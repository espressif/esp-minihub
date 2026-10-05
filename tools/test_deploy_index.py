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


def main() -> int:
    with TemporaryDirectory() as tmp:
        deploy = Path(tmp)
        assert store_paths.store_buckets(["esp32s31"]) == ["ESP32S31"]
        assert store_paths.store_buckets(["ALL"]) == ["Common"]
        assert store_paths.store_buckets(["all", "ESP32S31"]) == ["Common"]
        assert store_paths.store_buckets(["ESP32S31", "ESP32C6"]) == ["ESP32S31", "ESP32C6"]

        def _plant(mid: str, manifest: dict) -> None:
            folder = deploy / "Stores" / "Common" / mid
            folder.mkdir(parents=True)
            with zipfile.ZipFile(folder / f"{mid}.mhub", "w") as zf:
                zf.writestr("module.json", json.dumps(manifest))

        # Plant every package under Common. Rebuild must move chip targets.
        _plant(
            "demo",
            {
                "id": "demo",
                "name": "Demo",
                "version": "1.2.0",
                "runtime": "js",
                "vendor": "lab",
                "description": "demo",
                "targets": ["ESP32S31"],
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
        _plant(
            "multi",
            {
                "id": "multi",
                "name": "Multi",
                "version": "1.0.0",
                "runtime": "native",
                "targets": ["esp32s31", "ESP32C6"],
            },
        )
        catalog = sync_store_catalog.rebuild_catalog_from_store(deploy)
        by_id = {item["id"]: item for item in catalog}
        assert set(by_id) == {"demo", "jsdemo", "multi"}, catalog
        demo_pkg = deploy / "Stores" / "ESP32S31" / "demo" / "demo.mhub"
        assert demo_pkg.is_file()
        assert not (deploy / "Stores" / "Common" / "demo").exists()
        assert not (deploy / "Stores" / "ESP32S31" / "jsdemo").exists()
        assert (deploy / "Stores" / "Common" / "jsdemo" / "jsdemo.mhub").is_file()
        assert (deploy / "Stores" / "ESP32S31" / "multi" / "multi.mhub").is_file()
        assert (deploy / "Stores" / "ESP32C6" / "multi" / "multi.mhub").is_file()
        assert not (deploy / "Stores" / "Common" / "multi").exists()
        assert by_id["demo"]["url"] == "/files/Stores/ESP32S31/demo/demo.mhub"
        assert by_id["demo"]["sha256"] == _sha(demo_pkg.read_bytes())
        assert by_id["demo"]["size"] == demo_pkg.stat().st_size
        assert by_id["demo"]["targets"] == ["ESP32S31"]
        assert by_id["jsdemo"]["url"] == "/files/Stores/Common/jsdemo/jsdemo.mhub"
        assert by_id["jsdemo"]["targets"] == ["ALL"]
        assert by_id["multi"]["targets"] == ["esp32s31", "ESP32C6"]
        assert by_id["multi"]["url"] == "/files/Stores/ESP32S31/multi/multi.mhub"
        per = json.loads(
            (deploy / "Stores" / "ESP32S31" / "demo" / "catalog.json").read_text(encoding="utf-8")
        )
        assert per["id"] == "demo"
        assert per["targets"] == ["ESP32S31"]
        c6 = json.loads(
            (deploy / "Stores" / "ESP32C6" / "multi" / "catalog.json").read_text(encoding="utf-8")
        )
        assert c6["url"] == "/files/Stores/ESP32C6/multi/multi.mhub"
        plugin = deploy / "Stores" / "ESP32S31" / "demo"
        kept = sync_store_catalog.catalog_entry(
            {
                "id": "other",
                "name": "Other",
                "version": "1.0.0",
                "runtime": "js",
                "targets": ["ESP32C6"],
            },
            plugin,
            plugin,
        )
        assert kept["targets"] == ["ESP32C6"]
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
        assert native_default["targets"] == ["ESP32S31"]

        fw = deploy / "Firmwares" / "ESP32S31" / "0.1.0"
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
            "ESP32S31",
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
                    "ESP32S31",
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
                note_deploy / "Firmwares" / "ESP32S31" / "0.0.1" / "release_notes.txt"
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
        assert "window.mhWebVersion = 'v0.1.0'" in boot_js
        assert "old" not in boot_js
        assert (web_root / "Webs" / "v0.1.0" / "version.txt").read_text(encoding="utf-8") == "v0.1.0\n"
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
        ).read_text(encoding="utf-8") == "v0.1.0\n"
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
        assert fw_api[0]["targets"] == ["ESP32S31"]
        assert fw_api[0]["url"] == (
            "/files/Firmwares/ESP32S31/0.1.0/minihub-esp32s31-0.1.0.bin"
        )
        assert fw_api[0]["sha256"] == _sha((fw / "minihub-esp32s31-0.1.0.bin").read_bytes())
        assert fw_api[0]["release_note"] == note
        assert "json" not in fw_api[0]

        plugins = store_server.load_plugin_catalog(deploy)
        assert [item["id"] for item in plugins] == ["demo", "jsdemo", "multi"]
        assert next(item for item in plugins if item["id"] == "demo")["url"].startswith(
            "/files/Stores/ESP32S31/"
        )
        assert next(item for item in plugins if item["id"] == "jsdemo")["url"].startswith(
            "/files/Stores/Common/"
        )

        assert store_server.resolve_deploy_file(deploy, "/files/../secret.txt") is None
        assert store_server.resolve_deploy_file(deploy, "/files/Stores/../../secret.txt") is None
        assert store_server.resolve_deploy_file(deploy, "/files/%2e%2e/secret.txt") is None
        got = store_server.resolve_deploy_file(
            deploy, "/files/Firmwares/ESP32S31/0.1.0/minihub-esp32s31-0.1.0.bin"
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

            status, body = _get(base + "/api/catalog")
            assert status == 200, body
            listed = {item["id"]: item for item in json.loads(body)["plugins"]}
            assert listed["demo"]["targets"] == ["ESP32S31"]
            assert listed["jsdemo"]["targets"] == ["ALL"]
            assert "multi" in listed
            status_m, body_m = _get(base + "/api/catalog?mac=" + mac)
            assert status_m == 200, body_m
            assert json.loads(body_m) == json.loads(body)

            status, body = _get(base + "/api/firmware")
            assert status == 200
            assert json.loads(body)[0]["chip"] == "ESP32S31"
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
                "/files/Firmwares/ESP32S31/0.1.0/minihub-esp32s31-0.1.0.bin",
                "/files/Stores/ESP32S31/demo/demo.mhub",
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
