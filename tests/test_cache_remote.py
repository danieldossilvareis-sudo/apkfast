"""Testes unitários do cache portável (schema 2)."""

from __future__ import annotations

import io
import json
import os
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from apkfast import cache as c


class CacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "cache"
        self.root.mkdir()
        self.ctx = c.CacheContext(
            root=self.root,
            project_id=c.project_id("com.example.app", "main"),
            mode="verify",
            require_signature=False,
            abis=("arm64-v8a",),
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_project_id_is_stable_and_namespace_changes_it(self) -> None:
        a = c.project_id("com.example.app", "main")
        b = c.project_id("com.example.app", "main")
        n = c.project_id("com.example.app", "main", "variant")
        self.assertEqual(a, b)
        self.assertNotEqual(a, n)
        self.assertEqual(len(a), 64)

    def test_logical_paths_are_portable(self) -> None:
        roots = [("/home/u/proj", "<project>"), ("/opt/ndk", "<ndk>")]
        self.assertEqual(
            c.logical_path("/home/u/proj/jni/main.cpp", roots),
            "<project>/jni/main.cpp",
        )
        self.assertEqual(
            c.canonical_flags(
                ["-I/home/u/proj/jni", "--sysroot=/opt/ndk/sysroot", "-O2"], roots
            ),
            ["-I<project>/jni", "--sysroot=<ndk>/sysroot", "-O2"],
        )

    def test_blob_roundtrip_and_tamper_detection(self) -> None:
        digest = c.write_blob(self.ctx, b"objeto")
        self.assertEqual(c.read_blob(self.ctx, digest), b"objeto")
        path = self.ctx.blob_path(digest)
        path.write_bytes(b"corrompido")
        with self.assertRaises(c.CacheError):
            c.read_blob(self.ctx, digest)

    def test_elf_machine_check(self) -> None:
        header = bytearray(20)
        header[:4] = b"\x7fELF"
        header[4] = 2  # ELF64
        header[5] = 1  # little endian
        header[18:20] = (0xB7).to_bytes(2, "little")  # AArch64
        path = self.root / "fake.so"
        path.write_bytes(header)
        self.assertTrue(c.elf_matches(path, "arm64-v8a"))
        self.assertFalse(c.elf_matches(path, "x86_64"))

    def test_export_import_roundtrip(self) -> None:
        blob = c.write_blob(self.ctx, b"conteudo-do-objeto")
        elf = bytearray(20)
        elf[:4] = b"\x7fELF"
        elf[4], elf[5] = 2, 1
        elf[18:20] = (0xB7).to_bytes(2, "little")
        so_blob = c.write_blob(self.ctx, bytes(elf))
        cache_file = self.ctx.cache_file("arm64-v8a")
        cache_file.parent.mkdir(parents=True, exist_ok=True)
        cache_file.write_text(
            json.dumps(
                {
                    "schema": c.CACHE_SCHEMA,
                    "projectId": self.ctx.project_id,
                    "abi": "arm64-v8a",
                    "objects": {
                        "key1": {
                            "obj_blob": blob,
                            "src": "<project>/jni/main.cpp",
                            "src_hash": "x",
                        }
                    },
                    "link": {"key": "k", "so_blob": so_blob, "lib": "main"},
                }
            )
        )

        bundle = self.root.parent / "bundle.tar.gz"
        manifest = c.export_bundle(self.ctx, bundle, ["arm64-v8a"])
        self.assertEqual(len(manifest["blobs"]), 2)

        other_root = self.root.parent / "other"
        other = c.CacheContext(
            root=other_root,
            project_id=self.ctx.project_id,
            mode="verify",
            require_signature=False,
            abis=("arm64-v8a",),
        )
        stats = c.import_bundle(other, bundle, abis=["arm64-v8a"])
        self.assertEqual(stats["imported"], 2)
        self.assertEqual(stats["rejected"], 0)
        self.assertTrue(other.cache_file("arm64-v8a").is_file())

    def test_hmac_signature(self) -> None:
        payload = {"a": 1, "b": [2, 3]}
        with patch.dict(os.environ, {"APKFAST_CACHE_HMAC": "segredo"}):
            c.sign_manifest(payload)
            c.verify_manifest(payload, required=True)
            payload["a"] = 999
            with self.assertRaises(c.CacheError):
                c.verify_manifest(payload, required=True)

    def test_unsafe_tar_member_is_rejected(self) -> None:
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w:gz") as tar:
            info = tarfile.TarInfo("../evil")
            info.size = 4
            tar.addfile(info, io.BytesIO(b"evil"))
        bundle = self.root.parent / "evil.tar.gz"
        bundle.write_bytes(buffer.getvalue())
        with self.assertRaises(c.CacheError):
            c.import_bundle(self.ctx, bundle, abis=["arm64-v8a"])


if __name__ == "__main__":
    unittest.main()
