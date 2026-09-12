#!/usr/bin/env python3
"""Host tests for canonical installed state and transaction recovery."""

from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import sys
from typing import Any, Callable


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location(
    "phipia_transaction", ROOT / "tools" / "phipia-transaction.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("could not load transaction tool")
TRANSACTION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TRANSACTION)


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest().upper()


PUBLISHER_KEY_ID = digest(b"publisher public key")


def package(identifier: str, version: str, path: str, payload: bytes, *,
            explicit: bool, dependencies: list[dict[str, str]] | None = None,
            kind: str = "library") -> dict[str, Any]:
    return {
        "identifier": identifier,
        "version": version,
        "package_sha256": digest(
            identifier.encode("ascii") + b"\0" + version.encode("ascii") + payload),
        "publisher_key_id": PUBLISHER_KEY_ID,
        "explicit": explicit,
        "dependencies": dependencies or [],
        "files": [{
            "path": path,
            "kind": kind,
            "mode": 0o555 if kind == "executable" else 0o444,
            "soname": path.rsplit("/", 1)[-1] if kind == "library" else "",
            "payload": payload,
        }],
    }


def dependency(identifier: str, provider: str | None = None) -> dict[str, str]:
    return {
        "identifier": identifier,
        "constraint": "^1.0.0",
        "provider": provider or identifier,
    }


def expect_refusal(action: Callable[[], Any], contains: str) -> None:
    try:
        action()
    except TRANSACTION.TransactionError as error:
        assert contains in str(error), str(error)
        return
    raise AssertionError(f"expected refusal containing {contains!r}")


def commit_install(store: Any, packages: list[dict[str, Any]]) -> None:
    TRANSACTION.stage_install(store, packages)
    assert TRANSACTION.commit(store) == "committed"
    assert TRANSACTION.verify(store) == []


def test_canonical_database_authority_and_journal() -> None:
    lib = package("org.phipia.lib", "1.0.0", "lib/libx.so.1", b"library",
                  explicit=False)
    app = package(
        "org.phipia.app", "1.0.0", "bin/app", b"application", explicit=True,
        dependencies=[dependency("org.phipia.lib")], kind="executable")
    store = TRANSACTION.create_store(capacity_bytes=1_000_000)
    journal_report = TRANSACTION.stage_install(store, [lib, app])
    assert journal_report["operation"] == "install"
    assert journal_report["base_generation"] == 1
    assert journal_report["target_generation"] == 2
    assert TRANSACTION.parse_journal(store.journal) == journal_report
    generation, snapshot = store.staged
    assert generation == 2
    report = TRANSACTION.parse_database(snapshot.database)
    assert [item["identifier"] for item in report["packages"]] == [
        "org.phipia.app", "org.phipia.lib"]
    assert TRANSACTION.encode_database(
        generation=2, architecture="x86_64", abi=1,
        packages=list(reversed(report["packages"]))) == snapshot.database
    authority = TRANSACTION.encode_authority(snapshot.database)
    assert TRANSACTION.parse_authority(authority)["generation"] == 2

    changed = bytearray(snapshot.database)
    changed[136] = 1
    expect_refusal(lambda: TRANSACTION.parse_database(bytes(changed)), "reserved bytes")
    changed = bytearray(store.journal)
    changed[-1] = 1
    expect_refusal(lambda: TRANSACTION.parse_journal(bytes(changed)), "reserved bytes")
    changed = bytearray(authority)
    changed[96] = 1
    expect_refusal(lambda: TRANSACTION.parse_authority(bytes(changed)), "reserved bytes")


def test_interrupted_commit_selects_only_complete_old_or_new() -> None:
    candidate = package(
        "org.phipia.app", "1.0.0", "bin/app", b"v1", explicit=True,
        kind="executable")
    old_store = TRANSACTION.create_store(capacity_bytes=1_000_000)
    TRANSACTION.stage_install(old_store, [candidate])
    assert TRANSACTION.commit(old_store, interrupt="before-authority") \
        == "interrupted-before-authority"
    assert TRANSACTION.recover(old_store) == "old"
    assert TRANSACTION.installed(old_store)["packages"] == []
    assert old_store.journal is None and old_store.staged is None

    new_store = TRANSACTION.create_store(capacity_bytes=1_000_000)
    TRANSACTION.stage_install(new_store, [candidate])
    assert TRANSACTION.commit(new_store, interrupt="after-authority") \
        == "interrupted-after-authority"
    assert TRANSACTION.recover(new_store) == "new"
    assert [item["identifier"] for item in TRANSACTION.installed(new_store)["packages"]] \
        == ["org.phipia.app"]
    assert len(new_store.generations) == 1


def test_update_rollback_and_cancellation_cleanup() -> None:
    store = TRANSACTION.create_store(capacity_bytes=1_000_000)
    version_one = package(
        "org.phipia.app", "1.0.0", "bin/app", b"version one", explicit=True,
        kind="executable")
    commit_install(store, [version_one])
    base_generation = TRANSACTION.installed(store)["generation"]
    base_used = TRANSACTION.used_space(store)
    version_two = package(
        "org.phipia.app", "2.0.0", "bin/app", b"version two", explicit=True,
        kind="executable")
    report = TRANSACTION.stage_install(store, [version_two])
    assert report["operation"] == "update"
    assert TRANSACTION.commit(store, interrupt="before-authority") \
        == "interrupted-before-authority"
    assert TRANSACTION.recover(store) == "old"
    installed = TRANSACTION.installed(store)
    assert installed["generation"] == base_generation
    assert installed["packages"][0]["version"] == "1.0.0"
    assert next(iter(store.generations.values())).files["bin/app"] == b"version one"

    TRANSACTION.stage_install(store, [version_two])
    assert TRANSACTION.used_space(store) > base_used
    TRANSACTION.cancel(store)
    assert TRANSACTION.used_space(store) == base_used
    assert store.journal is None and store.staged is None


def test_remove_keeps_shared_dependency_and_preserves_user_data() -> None:
    user_data = {"org.phipia.app-a/settings.db": b"personal settings"}
    store = TRANSACTION.create_store(
        capacity_bytes=2_000_000, user_data=user_data)
    lib = package("org.phipia.lib", "1.0.0", "lib/libshared.so.1", b"shared",
                  explicit=False)
    app_a = package(
        "org.phipia.app-a", "1.0.0", "bin/app-a", b"app a", explicit=True,
        dependencies=[dependency("org.phipia.lib")], kind="executable")
    app_b = package(
        "org.phipia.app-b", "1.0.0", "bin/app-b", b"app b", explicit=True,
        dependencies=[dependency("org.phipia.lib")], kind="executable")
    commit_install(store, [lib, app_a, app_b])
    TRANSACTION.stage_remove(store, ["org.phipia.app-a"])
    assert TRANSACTION.commit(store, interrupt="after-authority") \
        == "interrupted-after-authority"
    assert TRANSACTION.recover(store) == "new"
    assert [item["identifier"] for item in TRANSACTION.installed(store)["packages"]] == [
        "org.phipia.app-b", "org.phipia.lib"]
    assert "lib/libshared.so.1" in next(iter(store.generations.values())).files
    assert store.user_data == user_data

    TRANSACTION.stage_remove(store, ["org.phipia.app-b"])
    assert TRANSACTION.commit(store) == "committed"
    assert TRANSACTION.installed(store)["packages"] == []
    assert store.user_data == user_data


def test_collision_low_space_and_transaction_refusals() -> None:
    store = TRANSACTION.create_store(capacity_bytes=1_000_000)
    first = package("org.phipia.one", "1.0.0", "lib/shared", b"one",
                    explicit=True)
    commit_install(store, [first])
    second = package("org.phipia.two", "1.0.0", "lib/shared", b"two",
                     explicit=True)
    expect_refusal(lambda: TRANSACTION.stage_install(store, [second]),
                   "file ownership collision")
    assert store.journal is None and store.staged is None

    tiny = TRANSACTION.create_store(capacity_bytes=700)
    expect_refusal(lambda: TRANSACTION.stage_install(tiny, [first]),
                   "insufficient space")
    assert TRANSACTION.installed(tiny)["packages"] == []
    assert tiny.journal is None and tiny.staged is None

    cycle_a = package(
        "org.phipia.a", "1.0.0", "lib/a", b"a", explicit=True,
        dependencies=[dependency("org.phipia.b")])
    cycle_b = package(
        "org.phipia.b", "1.0.0", "lib/b", b"b", explicit=False,
        dependencies=[dependency("org.phipia.a")])
    expect_refusal(lambda: TRANSACTION.stage_install(store, [cycle_a, cycle_b]), "cycle")


def test_verify_and_repair_preserve_user_data() -> None:
    user_data = {"org.phipia.app/document.txt": b"do not replace"}
    store = TRANSACTION.create_store(
        capacity_bytes=1_000_000, user_data=user_data)
    payload = b"known authenticated application"
    app = package(
        "org.phipia.app", "1.0.0", "bin/app", payload, explicit=True,
        kind="executable")
    commit_install(store, [app])
    snapshot = next(iter(store.generations.values()))
    snapshot.files["bin/app"] = b"corrupt"
    assert TRANSACTION.verify(store) == ["size:bin/app"]
    expect_refusal(lambda: TRANSACTION.stage_repair(store, {}),
                   "no authenticated repair payload")
    report = TRANSACTION.stage_repair(store, {"bin/app": payload})
    assert report["operation"] == "repair"
    assert TRANSACTION.commit(store) == "committed"
    assert TRANSACTION.verify(store) == []
    assert store.user_data == user_data


def test_recovery_falls_back_from_incomplete_new_generation() -> None:
    store = TRANSACTION.create_store(capacity_bytes=1_000_000)
    version_one = package(
        "org.phipia.app", "1.0.0", "bin/app", b"one", explicit=True,
        kind="executable")
    commit_install(store, [version_one])
    version_two = package(
        "org.phipia.app", "2.0.0", "bin/app", b"two", explicit=True,
        kind="executable")
    TRANSACTION.stage_install(store, [version_two])
    assert TRANSACTION.commit(store, interrupt="after-authority") \
        == "interrupted-after-authority"
    new_generation = TRANSACTION.parse_authority(store.authority)["generation"]
    store.generations[new_generation].files["bin/app"] = b"damaged"
    assert TRANSACTION.recover(store) == "old"
    assert TRANSACTION.installed(store)["packages"][0]["version"] == "1.0.0"


def main() -> int:
    test_canonical_database_authority_and_journal()
    test_interrupted_commit_selects_only_complete_old_or_new()
    test_update_rollback_and_cancellation_cleanup()
    test_remove_keeps_shared_dependency_and_preserves_user_data()
    test_collision_low_space_and_transaction_refusals()
    test_verify_and_repair_preserve_user_data()
    test_recovery_falls_back_from_incomplete_new_generation()
    print(
        "Phipia transaction host tests passed: canonical state, staging, atomic recovery, "
        "rollback, remove/refcounts, collisions, cancellation, low-space, repair, user data")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
