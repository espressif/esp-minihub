#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 MiniHub Contributors
# SPDX-License-Identifier: Apache-2.0
"""Check deploy-root catalog aggregation, firmware walk, and /files/ traversal."""

from __future__ import annotations

import hashlib
import io
import json
import threading
import urllib.error
import urllib.request
import zipfile
from contextlib import redirect_stderr
from http.server import ThreadingHTTPServer
from pathlib import Path
from tempfile import TemporaryDirectory

import gen_deploy_index
import minify_web_assets
import publish_firmware
import store_paths
import store_server
import sync_store_catalog


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _get(url: str) -> tuple[int, bytes]:
    try:
        with urllib.request.urlopen(url, timeout=5) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read()


def _test_version_cmp() -> None:
    """Device and store_server share the same precedence rules."""
    cmp = store_server.version_cmp
    assert cmp("0.1.0", "0.1.0") == 0
    assert cmp("v0.1.0", "0.1.0") == 0
    assert cmp("0.1.0", "0.1.0-dev") > 0
    assert cmp("0.1.0", "0.1.0-pre") > 0
    assert cmp("0.1.0-pre", "0.1.0-beta") > 0
    assert cmp("0.1.0-beta", "0.1.0-dev") > 0
    assert cmp("0.1.0-pre", "0.1.0-pre1") < 0
    assert cmp("0.1.0-pre1", "0.1.0-pre2") < 0
    assert cmp("0.1.0-beta", "0.1.0-beta1") < 0
    assert cmp("0.1.0-dev", "0.1.0-dev1") < 0
    assert cmp("0.1.1", "0.1.0-dev") > 0
    assert cmp("0.1.0-dev", "0.1.0") < 0
    # Running 0.1.0-dev must see published 0.1.0 as newer.
    with TemporaryDirectory() as tmp:
        deploy = Path(tmp)
        fw = deploy / "Firmwares" / "esp32s31" / "0.1.0"
        fw.mkdir(parents=True)
        (fw / "minihub.bin").write_bytes(b"fw")
        (fw / "boards.txt").write_text("ESP32-S31-Function-Coreboard-1\n", encoding="utf-8")
        up = store_server.build_firmware_upgrade(deploy, "esp32s31", "0.1.0-dev")
        assert up["newer"] is True
        assert up["version"] == "0.1.0"
        up2 = store_server.build_firmware_upgrade(deploy, "esp32s31", "0.1.0")
        assert up2["newer"] is False
        # board= with matching boards.txt
        up3 = store_server.build_firmware_upgrade(
            deploy, "esp32s31", "0.1.0-dev", board="ESP32-S31-Function-Coreboard-1"
        )
        assert up3["newer"] is True
        # Korvo-1 must not see a Function-Coreboard-only package
        up4 = store_server.build_firmware_upgrade(
            deploy, "esp32s31", "0.1.0-dev", board="ESP32-S31-Korvo-1"
        )
        assert up4["available"] is False
        assert up4["newer"] is False
        # Legacy folder without boards.txt: chip-wide only when board omitted
        legacy = deploy / "Firmwares" / "esp32s31" / "0.0.9"
        legacy.mkdir(parents=True)
        (legacy / "minihub.bin").write_bytes(b"old")
        assert store_server.build_firmware_upgrade(
            deploy, "esp32s31", "0.0.8", board="ESP32-S31-Korvo-1"
        )["available"] is False
        # Without board=, legacy 0.0.9 is visible but 0.1.0 still wins
        up5 = store_server.build_firmware_upgrade(deploy, "esp32s31", "0.0.8")
        assert up5["version"] == "0.1.0"


def main() -> int:
    _test_version_cmp()
    with TemporaryDirectory() as tmp:
        deploy = Path(tmp)
        assert store_paths.store_buckets(["esp32s31"]) == ["esp32s31"]
        assert store_paths.store_buckets(["ALL"]) == ["Common"]
        assert store_paths.store_buckets(["all", "esp32s31"]) == ["Common"]
        assert store_paths.store_buckets(["esp32s31", "esp32c6"]) == ["esp32s31", "esp32c6"]

        def _plant(mid: str, manifest: dict) -> None:
            folder = deploy / "Stores" / "Common" / mid
            folder.mkdir(parents=True)
            with zipfile.ZipFile(folder / f"{mid}.mhub", "w") as zf:
                zf.writestr("module.json", json.dumps(manifest))

        # L1/L2 stay under Stores/Common. A native package is removed and
        # is not filed under Common or a chip folder.
        _plant(
            "demo",
            {
                "id": "demo",
                "name": "Demo",
                "version": "1.2.0",
                "runtime": "js",
                "vendor": "lab",
                "description": "demo",
                "targets": ["esp32s31"],
            },
        )
        _plant(
            "jsdemo",
            {
                "id": "jsdemo",
                "name": "JS",
                "version": "1.0.0",
                "runtime": "js",
                "targets": ["ALL"],
            },
        )
        with zipfile.ZipFile(deploy / "Stores" / "Common" / "jsdemo" / "jsdemo.mhub", "a") as zf:
            zf.writestr(
                "meta.json",
                json.dumps({"ui": {"filter_label": "JS"}, "capabilities": ["points"]}),
            )
        _plant(
            "multi",
            {
                "id": "multi",
                "name": "Multi",
                "version": "1.0.0",
                "runtime": "native",
                "targets": ["esp32s31", "esp32c6"],
            },
        )
        leftover = deploy / "Stores" / "esp32s31" / "multi"
        leftover.mkdir(parents=True)
        (leftover / "multi.mhub").write_bytes(
            (deploy / "Stores" / "Common" / "multi" / "multi.mhub").read_bytes()
        )
        catalog = sync_store_catalog.rebuild_catalog_from_store(deploy)
        by_id = {item["id"]: item for item in catalog}
        assert set(by_id) == {"demo", "jsdemo"}, catalog
        demo_pkg = deploy / "Stores" / "Common" / "demo" / "demo.mhub"
        assert demo_pkg.is_file()
        assert not (deploy / "Stores" / "esp32s31").exists()
        assert not (deploy / "Stores" / "esp32c6").exists()
        assert not (deploy / "Stores" / "Common" / "multi").exists()
        assert (deploy / "Stores" / "Common" / "jsdemo" / "jsdemo.mhub").is_file()
        assert by_id["demo"]["url"] == "/files/Stores/Common/demo/demo.mhub"
        assert by_id["demo"]["sha256"] == _sha(demo_pkg.read_bytes())
        assert by_id["demo"]["size"] == demo_pkg.stat().st_size
        assert by_id["demo"]["targets"] == ["esp32s31"]
        # JS plugins are ABI-independent: the catalog omits the abi block.
        assert "abi" not in by_id["demo"]
        assert "abi" not in by_id["jsdemo"]
        assert "ui" not in by_id["jsdemo"]
        assert "capabilities" not in by_id["jsdemo"]
        assert "sha256:" not in by_id["demo"]["sha256"]
        assert by_id["jsdemo"]["url"] == "/files/Stores/Common/jsdemo/jsdemo.mhub"
        assert by_id["jsdemo"]["targets"] == ["ALL"]
        per = json.loads(
            (deploy / "Stores" / "Common" / "demo" / "catalog.json").read_text(encoding="utf-8")
        )
        assert per["id"] == "demo"
        assert per["targets"] == ["esp32s31"]
        plugin = deploy / "Stores" / "Common" / "demo"
        kept = sync_store_catalog.catalog_entry(
            {
                "id": "other",
                "name": "Other",
                "version": "1.0.0",
                "runtime": "js",
                "targets": ["esp32c6"],
            },
            plugin,
            plugin,
        )
        assert kept["targets"] == ["esp32c6"]
        defaulted = sync_store_catalog.catalog_entry(
            {"id": "plain", "name": "Plain", "version": "1.0.0", "runtime": "js"},
            plugin,
            plugin,
        )
        assert defaulted["targets"] == ["ALL"]
        native_default = sync_store_catalog.catalog_entry(
            {"id": "native", "name": "Native", "version": "1.0.0", "runtime": "native"},
            plugin,
            plugin,
        )
        assert native_default["targets"] == ["esp32s31"]
        assert "url" not in native_default

        fw = deploy / "Firmwares" / "esp32s31" / "0.1.0"
        note = "first cut\n"
        bin_src = deploy / "src.bin"
        bin_src.write_bytes(b"firmware-bytes")
        notes_file = deploy / "release_notes.txt"
        notes_file.write_text("first cut", encoding="utf-8")

        def _reject(argv: list[str], needle: str) -> None:
            err = io.StringIO()
            with redirect_stderr(err):
                try:
                    publish_firmware.main(argv)
                except SystemExit as exc:
                    assert exc.code == 2, argv
                else:
                    raise AssertionError(f"expected failure: {argv}")
            assert needle in err.getvalue(), err.getvalue()

        base_argv = [
            "--bin",
            str(bin_src),
            "--chip",
            "esp32s31",
            "--version",
            "0.1.0",
            "--deploy",
            str(deploy),
        ]
        _reject(base_argv, "exactly one of --note")
        _reject(base_argv + ["--note", note, "--notes", str(notes_file)], "exactly one of --note")
        _reject(
            base_argv + ["--notes", str(deploy / "missing_release_notes.txt")],
            "release_notes.txt",
        )
        with TemporaryDirectory() as note_tmp:
            note_deploy = Path(note_tmp)
            decoy = note_deploy / "release_notes.txt"
            decoy.write_text("file body\n", encoding="utf-8")
            (note_deploy / "src.bin").write_bytes(b"x")
            rc = publish_firmware.main(
                [
                    "--bin",
                    str(note_deploy / "src.bin"),
                    "--chip",
                    "esp32s31",
                    "--version",
                    "0.0.1",
                    "--note",
                    str(decoy),
                    "--deploy",
                    str(note_deploy),
                ]
            )
            assert rc == 0
            written = (
                note_deploy / "Firmwares" / "esp32s31" / "0.0.1" / "release_notes.txt"
            ).read_text(encoding="utf-8")
            assert written == str(decoy) + "\n"
            assert "file body" not in written

        rc = publish_firmware.main(
            base_argv
            + [
                "--notes",
                str(notes_file),
                "--name",
                "minihub-esp32s31-0.1.0.bin",
            ]
        )
        assert rc == 0
        assert not (fw / "index.json").exists()
        assert (fw / "release_notes.txt").read_text(encoding="utf-8") == note
        assert not (fw / "release_note.txt").exists()
        assert not list(fw.glob("*.json"))

        (deploy / "Assets" / "model").mkdir(parents=True)
        csv = deploy / "Assets" / "model" / "devices.csv"
        csv.write_text("id,name\n", encoding="utf-8")
        (deploy / "Webs" / "0.1.0").mkdir(parents=True)
        (deploy / "Webs" / "0.1.0" / "index.html").write_text("<p>ok</p>", encoding="utf-8")
        web_note = "served note\n"
        (deploy / "Webs" / "0.1.0" / "release_notes.txt").write_text(web_note, encoding="utf-8")

        web_src = deploy / "web_src"
        (web_src / "js").mkdir(parents=True)
        (web_src / "index.html").write_text("<p>ok</p>\n", encoding="utf-8")
        (web_src / "js" / "boot.js").write_text(
            "// drop me\nwindow.mhWebVersion = 'old';\nconst x = 1;\n",
            encoding="utf-8",
        )
        (web_src / "release_notes.txt").write_text("from src\n", encoding="utf-8")
        web_notes = deploy / "web_release_notes.txt"
        web_notes.write_text("console notes\n", encoding="utf-8")

        def _reject_web(argv: list[str], needle: str) -> None:
            try:
                minify_web_assets.main(argv)
            except SystemExit as exc:
                assert needle in str(exc.code), exc
            else:
                raise AssertionError(f"expected failure: {argv}")

        _reject_web(
            ["--src", str(web_src), "--dst", str(deploy / "web_dst"), "--notes", str(web_notes)],
            "--notes requires",
        )
        _reject_web(
            [
                "--src",
                str(web_src),
                "--deploy-root",
                str(deploy / "web_missing"),
                "--web-version",
                "v0.1.0",
                "--notes",
                str(deploy / "missing_web_notes.txt"),
                "-q",
            ],
            "release_notes.txt",
        )
        web_root = deploy / "web_notes_root"
        rc = minify_web_assets.main(
            [
                "--src",
                str(web_src),
                "--deploy-root",
                str(web_root),
                "--web-version",
                "v0.1.0",
                "--notes",
                str(web_notes),
                "-q",
            ]
        )
        assert rc == 0
        published = web_root / "Webs" / "v0.1.0" / "release_notes.txt"
        assert published.read_bytes() == b"console notes\n"
        boot_js = (web_root / "Webs" / "v0.1.0" / "js" / "boot.js").read_text(encoding="utf-8")
        assert "drop me" not in boot_js
        assert "const x = 1;" in boot_js
        # Stamped files carry the canonical version (no "v"); only the Webs/
        # folder name does.
        assert "window.mhWebVersion = '0.1.0'" in boot_js
        assert "old" not in boot_js
        assert (web_root / "Webs" / "v0.1.0" / "version.txt").read_text(encoding="utf-8") == "0.1.0\n"
        bare = deploy / "web_bare"
        bare.mkdir()
        (bare / "index.html").write_text("<p>ok</p>\n", encoding="utf-8")
        rc = minify_web_assets.main(
            [
                "--src",
                str(bare),
                "--deploy-root",
                str(deploy / "web_plain_root"),
                "--web-version",
                "v0.1.0",
                "-q",
            ]
        )
        assert rc == 0
        assert not (deploy / "web_plain_root" / "Webs" / "v0.1.0" / "release_notes.txt").exists()
        assert (
            deploy / "web_plain_root" / "Webs" / "v0.1.0" / "version.txt"
        ).read_text(encoding="utf-8") == "0.1.0\n"
        # The Host build path (main/CMakeLists.txt) minifies to a plain dst with
        # no Webs/ folder: it must stamp the same canonical string, otherwise a
        # freshly flashed Hub reports a different version than the deploy tree.
        rc = minify_web_assets.main(
            [
                "--src",
                str(web_src),
                "--dst",
                str(deploy / "web_storage_image"),
                "--web-version",
                "0.1.0",
                "-q",
            ]
        )
        assert rc == 0
        storage_ver = deploy / "web_storage_image" / "version.txt"
        assert storage_ver.read_text(encoding="utf-8") == "0.1.0\n"
        assert "window.mhWebVersion = '0.1.0'" in (
            deploy / "web_storage_image" / "js" / "boot.js"
        ).read_text(encoding="utf-8")
        notes_idx = gen_deploy_index.build_web_index(web_root)
        by_url = {item["url"]: item for item in notes_idx["items"]}
        note_item = by_url["v0.1.0/release_notes.txt"]
        assert note_item["name"] == "release_notes.txt"
        assert note_item["version"] == "v0.1.0"
        assert note_item["type"] == "web"
        assert note_item["targets"] == ["*"]
        assert note_item["release_note"] == "console notes\n"
        assert note_item["size"] == published.stat().st_size
        assert note_item["sha256"] == _sha(published.read_bytes())
        assert "v0.1.0/index.html" in by_url
        assert "v0.1.0/js/boot.js" in by_url

        asset_idx = gen_deploy_index.build_asset_index(deploy)
        assert asset_idx["schema"] == "minihub.deploy.index/1"
        assert asset_idx["kind"] == "asset"
        assert asset_idx["base_url"] == "/files/Assets"
        assert asset_idx["items"][0]["sha256"] == _sha(csv.read_bytes())
        # Compact asset entry: only what the firmware reads. "id" is kept purely
        # for older parsers (update_index.c:116 rejects items without it); the
        # rest of _item()'s fields are dead weight on the device.
        assert set(asset_idx["items"][0]) == {"id", "url", "size", "sha256"}
        # Only the type catalogue is bulk-synced; avatars are fetched per device
        # type straight from /files/Assets/images/devices/<type>.svg, so listing
        # them would just be entries the device skips on every hourly pass.
        assert asset_idx["items"], "model/ assets must still be published"
        assert all(item["url"].startswith("model/") for item in asset_idx["items"])
        web_idx = gen_deploy_index.build_web_index(deploy)
        assert web_idx["kind"] == "web"
        assert web_idx["items"][0]["url"] == "0.1.0/index.html"
        assert web_idx["items"][0]["version"] == "0.1.0"
        served_note = next(item for item in web_idx["items"] if item["url"] == "0.1.0/release_notes.txt")
        assert served_note["name"] == "release_notes.txt"
        assert served_note["size"] == len(web_note.encode())
        assert served_note["sha256"] == _sha(web_note.encode())
        assert served_note["type"] == "web"
        assert served_note["release_note"] == web_note

        fw_api = store_server.load_firmware_catalog(deploy)
        assert len(fw_api) == 1
        assert fw_api[0]["version"] == "0.1.0"
        assert fw_api[0]["targets"] == ["esp32s31"]
        assert fw_api[0]["url"] == (
            "/files/Firmwares/esp32s31/0.1.0/minihub-esp32s31-0.1.0.bin"
        )
        assert fw_api[0]["sha256"] == _sha((fw / "minihub-esp32s31-0.1.0.bin").read_bytes())
        assert fw_api[0]["release_note"] == note
        assert "json" not in fw_api[0]
        (fw / "bootloader.bin").write_bytes(b"boot")
        (fw / "merged.bin").write_bytes(b"merged")
        (fw / "minihub.elf").write_bytes(b"elf")
        fw_api = store_server.load_firmware_catalog(deploy)
        assert len(fw_api) == 1
        assert fw_api[0]["url"].endswith("/minihub-esp32s31-0.1.0.bin")
        upgrade = store_server.build_firmware_upgrade(deploy, "esp32s31")
        assert upgrade["firmware_url"].endswith("/minihub-esp32s31-0.1.0.bin")

        plugins = store_server.load_plugin_catalog(deploy)
        assert [item["id"] for item in plugins] == ["demo", "jsdemo"]
        assert next(item for item in plugins if item["id"] == "demo")["url"].startswith(
            "/files/Stores/Common/"
        )
        assert next(item for item in plugins if item["id"] == "jsdemo")["url"].startswith(
            "/files/Stores/Common/"
        )

        assert store_server.resolve_deploy_file(deploy, "/files/../secret.txt") is None
        assert store_server.resolve_deploy_file(deploy, "/files/Stores/../../secret.txt") is None
        assert store_server.resolve_deploy_file(deploy, "/files/%2e%2e/secret.txt") is None
        got = store_server.resolve_deploy_file(
            deploy, "/files/Firmwares/esp32s31/0.1.0/minihub-esp32s31-0.1.0.bin"
        )
        assert got is not None and got.read_bytes() == b"firmware-bytes"

        handler = store_server.make_handler(deploy)
        srv = ThreadingHTTPServer(("127.0.0.1", 0), handler)
        port = srv.server_address[1]
        thread = threading.Thread(target=srv.serve_forever, daemon=True)
        thread.start()
        try:
            base = f"http://127.0.0.1:{port}"
            mac = "aa:bb:cc:dd:ee:ff"
            assert store_server.request_mac("/api/catalog?mac=" + mac) == mac
            assert store_server.request_mac("/api/catalog") == ""
            assert store_server.request_mac("/api/firmware?x=1&mac=" + mac) == mac

            from abi_contract_lib import (
                build_host_symbols,
                compute_abi_total_hash,
                parse_abi_version,
            )

            header = Path(__file__).resolve().parents[1] / "components/minihub_module/include/minihub_module_abi.h"
            allowlist = Path(__file__).resolve().parents[1] / "docs/module_sdk/host_export_allowlist.json"
            host_symbols = build_host_symbols(header, allowlist)
            abi_version = parse_abi_version(header)
            abi_total = compute_abi_total_hash(host_symbols)

            status, body = _get(base + "/api/catalog")
            assert status == 200, body

            status, body = _get(
                base + f"/api/catalog?abi_version={abi_version + 9}&abi_total_hash={abi_total}"
            )
            assert json.loads(body)["reason"] == "abi_version_unknown"

            status, body = _get(
                base + f"/api/catalog?abi_version={abi_version}&abi_total_hash=nope"
            )
            assert json.loads(body)["reason"] == "abi_mismatch"

            query = (
                f"/api/catalog?abi_version={abi_version}&abi_total_hash={abi_total}"
                f"&chip=esp32s31&mac={mac}"
            )
            status, body = _get(base + query)
            assert status == 200, body
            payload = json.loads(body)
            assert payload["status"] == "ok"
            listed = {item["id"]: item for item in payload["plugins"]}
            assert listed["demo"]["targets"] == ["esp32s31"]
            # JS plugins omit abi yet are still listed (ABI-independent).
            assert "abi" not in listed["demo"]
            assert listed["jsdemo"]["targets"] == ["ALL"]
            assert "multi" not in listed
            upgrade = payload["firmware_upgrade"]
            assert upgrade["available"] is True
            assert upgrade["version"] == "0.1.0"
            assert upgrade["firmware_url"].startswith("/files/Firmwares/esp32s31/0.1.0/")

            status, body = _get(base + "/api/firmware")
            assert status == 200
            assert json.loads(body)[0]["chip"] == "esp32s31"
            status_m, body_m = _get(base + "/api/firmware?mac=" + mac)
            assert status_m == 200, body_m
            assert json.loads(body_m) == json.loads(body)

            status, body = _get(base + "/api/assets")
            assert status == 200
            assert json.loads(body)["kind"] == "asset"
            status_m, body_m = _get(base + "/api/assets?mac=" + mac)
            assert status_m == 200, body_m
            assert body_m == body
            status, body = _get(base + "/api/web")
            assert status == 200
            assert json.loads(body)["kind"] == "web"
            status_m, body_m = _get(base + "/api/web?mac=" + mac)
            assert status_m == 200, body_m
            assert json.loads(body_m) == json.loads(body)
            for path in (
                "/files/Assets/model/devices.csv",
                "/files/Webs/0.1.0/index.html",
                "/files/Webs/0.1.0/release_notes.txt",
                "/files/Firmwares/esp32s31/0.1.0/minihub-esp32s31-0.1.0.bin",
                "/files/Stores/Common/demo/demo.mhub",
                "/files/Stores/Common/jsdemo/jsdemo.mhub",
            ):
                status_plain, body_plain = _get(base + path)
                assert status_plain == 200, path
                status_mac, body_mac = _get(base + path + "?mac=" + mac)
                assert status_mac == 200, path
                assert body_mac == body_plain
            needle = mac.encode()
            for path in deploy.rglob("*"):
                if path.is_file():
                    assert needle not in path.read_bytes(), path
            status, body = _get(base + "/files/Stores/../../secret.txt")
            assert status == 404, status
            status, _ = _get(base + "/files/%2e%2e/secret.txt")
            assert status == 404
        finally:
            srv.shutdown()

    print("test_deploy_index: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
