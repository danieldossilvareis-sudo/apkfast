"""Testes puros do cache de recursos/APK (sem SDK/NDK e sem subprocess real)."""

from __future__ import annotations

import json
import tempfile
import unittest
import zipfile
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from apkfast import cache as cache_mod
from apkfast import cli
from apkfast import resource_cache as rc
from apkfast.builder import Logger


class _FakeToolchain:
    def __init__(self, root: Path) -> None:
        self.aapt2 = root / "tools" / "aapt2"
        self.aapt2.parent.mkdir(parents=True, exist_ok=True)
        self.aapt2.write_text('#!/bin/sh\necho "aapt2 fake 2.20"\n')
        self.aapt2.chmod(0o755)
        self.zipalign = root / "tools" / "zipalign"
        self.zipalign.write_bytes(b"fake-zipalign")
        self.apksigner = root / "tools" / "apksigner"
        self.apksigner.write_bytes(b"fake-apksigner")
        self.ndk_dir = root / "ndk"
        self.sysroot = self.ndk_dir / "sysroot"
        self.abi = "arm64-v8a"
        self.api = 36

    def native_app_glue_dir(self) -> Path:
        return self.ndk_dir / "sources" / "android" / "native_app_glue"

    def set_aapt2_version(self, text: str) -> None:
        self.aapt2.write_text(f'#!/bin/sh\necho "{text}"\n')
        self.aapt2.chmod(0o755)


class ResourceCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.project = self.base / "proj"
        (self.project / "res" / "values").mkdir(parents=True)
        (self.project / "assets").mkdir(parents=True)
        (self.project / "AndroidManifest.xml").write_text(
            '<manifest xmlns:android="http://schemas.android.com/apk/res/android" '
            'package="com.example.app" android:versionCode="1" '
            'android:versionName="1.0"><uses-sdk android:minSdkVersion="21" '
            'android:targetSdkVersion="36"/></manifest>'
        )
        (self.project / "res" / "values" / "strings.xml").write_text("<resources/>")
        (self.project / "assets" / "data.txt").write_text("abc")
        self.tc = _FakeToolchain(self.base)
        self.manifest = SimpleNamespace(
            package="com.example.app", version_code="1", version_name="1.0"
        )
        self.options = SimpleNamespace(
            min_sdk=21,
            target_sdk=36,
            assets_dir=None,
            cache_namespace="",
            release=False,
            optimize="release",
            obfuscate=False,
            key_alias="androiddebugkey",
            bundletool_version="1.18.3",
            incremental=True,
        )
        self.ctx = self._context("main")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def _context(
        self,
        name: str,
        mode: str = "verify",
        abis: tuple[str, ...] = ("arm64-v8a",),
    ) -> cache_mod.CacheContext:
        return cache_mod.CacheContext(
            root=self.base / "cache",
            project_id=cache_mod.project_id("com.example.app", name),
            mode=mode,
            require_signature=False,
            abis=abis,
        )

    def _rkey(self) -> str:
        return rc.resource_key(self.project, self.manifest, self.options, self.tc, self.ctx)

    def _apk_root(self) -> Path:
        root = self.base / "work" / "apk"
        (root / "lib" / "arm64-v8a").mkdir(parents=True, exist_ok=True)
        native = root / "lib" / "arm64-v8a" / "libmain.so"
        dex = root / "classes.dex"
        if not native.exists():
            native.write_bytes(b"native-so")
        if not dex.exists():
            dex.write_bytes(b"dex-bytes")
        return root

    def _akey(self, keystore: Path | None = None, **kwargs: object) -> str:
        return rc.apk_key(
            self.project,
            self.manifest,
            self.options,
            self.tc,
            self.ctx,
            apk_root=self._apk_root(),
            lib_name="main",
            keystore=keystore,
            **kwargs,
        )

    def _keystore(self, name: str, payload: bytes) -> Path:
        path = self.base / name
        path.write_bytes(payload)
        return path

    def _zip(self, path: Path, entries: dict[str, bytes]) -> Path:
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for name, data in entries.items():
                archive.writestr(name, data)
        return path

    def _java_source(self, text: str = "class Main {}") -> Path:
        path = self.project / "src" / "Main.java"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def _dkey(self, **kwargs: object) -> str:
        return rc.dex_key(
            self.project, self.manifest, self.options, self.tc, self.ctx, **kwargs
        )

    def _aabkey(
        self, keystore: Path | None = None, **kwargs: object
    ) -> str:
        return rc.aab_key(
            self.project,
            self.manifest,
            self.options,
            self.tc,
            self.ctx,
            apk_root=self._apk_root(),
            lib_name="main",
            keystore=keystore,
            **kwargs,
        )

    def test_resource_key_is_stable_and_tracks_invalidators(self) -> None:
        base = self._rkey()
        self.assertEqual(len(base), 64)
        self.assertEqual(base, self._rkey())

        (self.project / "res" / "values" / "strings.xml").write_text("<resources>x</resources>")
        after_res = self._rkey()
        self.assertNotEqual(base, after_res)

        (self.project / "res" / "values" / "strings.xml").write_text("<resources/>")
        (self.project / "assets" / "data.txt").write_text("abcd")
        self.assertNotEqual(base, self._rkey())
        (self.project / "assets" / "data.txt").write_text("abc")

        manifest_path = self.project / "AndroidManifest.xml"
        text = manifest_path.read_text()
        manifest_path.write_text(text.replace('versionName="1.0"', 'versionName="1.1"'))
        self.assertNotEqual(base, self._rkey())
        manifest_path.write_text(text)

        self.options.min_sdk = 24
        self.assertNotEqual(base, self._rkey())
        self.options.min_sdk = 21

        self.tc.set_aapt2_version("aapt2 fake 3.0")
        self.assertNotEqual(base, self._rkey())

    def test_resource_key_includes_namespace_and_custom_assets(self) -> None:
        base = self._rkey()
        self.ctx = self._context("variant")
        self.assertNotEqual(base, self._rkey())
        self.ctx = self._context("main")
        custom = self.base / "other-assets"
        custom.mkdir()
        (custom / "x.txt").write_text("x")
        self.options.assets_dir = custom
        self.assertNotEqual(base, self._rkey())

    def test_apk_key_tracks_native_dex_and_keystore(self) -> None:
        ks1 = self._keystore("one.jks", b"keystore-one")
        ks2 = self._keystore("two.jks", b"keystore-two")
        base = self._akey(ks1)
        self.assertEqual(base, self._akey(ks1))
        self.assertNotEqual(base, self._akey(ks2))

        # A chave do APK unsigned não depende da keystore.
        self.assertEqual(self._akey(ks1, signed=False), self._akey(ks2, signed=False))
        self.assertNotEqual(self._akey(ks1), self._akey(ks1, signed=False))

        (self._apk_root() / "classes.dex").write_bytes(b"dex-changed")
        self.assertNotEqual(base, self._akey(ks1))
        (self._apk_root() / "classes.dex").write_bytes(b"dex-bytes")

        (self._apk_root() / "lib" / "arm64-v8a" / "libmain.so").write_bytes(b"native-2")
        self.assertNotEqual(base, self._akey(ks1))

    def test_resource_blob_hit_miss_and_tamper(self) -> None:
        key = self._rkey()
        flats_dir = self.base / "compiled"
        flats_dir.mkdir()
        flats = []
        for name, data in {"values_strings.arsc.flat": b"flat-a", "drawable_icon.flat": b"flat-b"}.items():
            path = flats_dir / name
            path.write_bytes(data)
            flats.append(path)
        digest = rc.store_resource(self.ctx, key, flats)
        self.assertIsNotNone(digest)

        destination = self.base / "restored"
        loaded = rc.load_resource(self.ctx, key, destination)
        self.assertEqual([p.name for p in loaded], [p.name for p in sorted(flats)])
        self.assertEqual(
            {p.name: p.read_bytes() for p in loaded},
            {p.name: p.read_bytes() for p in flats},
        )

        for path in flats:
            path.unlink()
        self.assertEqual(len(rc.load_resource(self.ctx, key, self.base / "restored2")), 2)

        with self.assertRaises(rc.CacheMiss):
            rc.load_resource(self.ctx, "0" * 64, destination)
        self.ctx.blob_path(digest).write_bytes(b"corrompido")  # type: ignore[arg-type]
        with self.assertRaises(rc.CacheMiss):
            rc.load_resource(self.ctx, key, destination)

    def test_apk_blob_hit_miss_and_tamper(self) -> None:
        key = self._akey(self._keystore("app.jks", b"key"))
        apk = self._zip(self.base / "app.apk", {"classes.dex": b"dex"})
        digest = rc.store_apk(self.ctx, key, apk)
        self.assertIsNotNone(digest)

        target = self.base / "work" / "signed.apk"
        self.assertEqual(rc.load_apk(self.ctx, key, target), target)
        self.assertEqual(target.read_bytes(), apk.read_bytes())

        with self.assertRaises(rc.CacheMiss):
            rc.load_apk(self.ctx, "1" * 64, target)
        with self.assertRaises(rc.CacheMiss):
            rc.load_apk(self.ctx, key, target, unsigned=True)
        self.ctx.blob_path(digest).write_bytes(b"nao-e-um-apk")  # type: ignore[arg-type]
        with self.assertRaises(rc.CacheMiss):
            rc.load_apk(self.ctx, key, target)

    def test_clear_is_scoped_by_project_and_removes_only_orphan_blobs(self) -> None:
        other = self._context("other")
        key_a = self._rkey()
        key_b = rc.resource_key(self.project, self.manifest, self.options, self.tc, other)

        flat = self.base / "values.arsc.flat"
        flat.write_bytes(b"flat-shared")
        digest = rc.store_resource(self.ctx, key_a, [flat])
        other_digest = rc.store_resource(other, key_b, [flat])
        self.assertEqual(digest, other_digest)

        ks = self._keystore("app.jks", b"key")
        apk = self._zip(self.base / "app.apk", {"AndroidManifest.xml": b"xml"})
        apk_key_a = self._akey(ks)
        apk_key_b = rc.apk_key(
            self.project, self.manifest, self.options, self.tc, other,
            apk_root=self._apk_root(), lib_name="main", keystore=ks,
        )
        apk_digest = rc.store_apk(self.ctx, apk_key_a, apk)
        rc.store_apk(other, apk_key_b, apk)

        rc.clear_resource_cache(self.ctx)
        rc.clear_apk_cache(self.ctx)
        with self.assertRaises(rc.CacheMiss):
            rc.load_resource(self.ctx, key_a, self.base / "gone")
        with self.assertRaises(rc.CacheMiss):
            rc.load_apk(self.ctx, apk_key_a, self.base / "gone.apk")
        # O conteúdo é compartilhado: outro projeto continua servindo o blob.
        self.assertEqual(rc.load_resource(other, key_b, self.base / "other-res")[0].read_bytes(), b"flat-shared")
        self.assertTrue(rc.load_apk(other, apk_key_b, self.base / "other.apk").is_file())
        self.assertTrue(self.ctx.blob_path(digest).is_file())  # type: ignore[arg-type]
        self.assertTrue(self.ctx.blob_path(apk_digest).is_file())  # type: ignore[arg-type]

        rc.clear_resource_cache(other)
        rc.clear_apk_cache(other)
        self.assertFalse(self.ctx.blob_path(digest).is_file())  # type: ignore[arg-type]
        self.assertFalse(self.ctx.blob_path(apk_digest).is_file())  # type: ignore[arg-type]

    def test_dex_key_tracks_sources_flags_and_tools(self) -> None:
        self._java_source()
        base = self._dkey()
        self.assertEqual(len(base), 64)
        self.assertEqual(base, self._dkey())

        self._java_source("class Main { int x = 1; }")
        self.assertNotEqual(base, self._dkey())

        self._java_source()
        self.options.optimize = "size"
        self.assertNotEqual(base, self._dkey())
        self.options.optimize = "release"

        self.options.obfuscate = True
        self.assertNotEqual(base, self._dkey())
        self.options.obfuscate = False

        self.options.min_sdk = 24
        self.assertNotEqual(base, self._dkey())
        self.options.min_sdk = 21

        with patch.object(rc, "java_tool_version", return_value="8.2.1"):
            v1 = self._dkey()
        with patch.object(rc, "java_tool_version", return_value="8.3.0"):
            self.assertNotEqual(v1, self._dkey())

    def test_aab_key_tracks_keystore_and_bundletool(self) -> None:
        self._java_source()
        ks1 = self._keystore("aab-one.jks", b"keystore-one")
        ks2 = self._keystore("aab-two.jks", b"keystore-two")
        base = self._aabkey(ks1)
        self.assertEqual(base, self._aabkey(ks1))
        self.assertNotEqual(base, self._aabkey(ks2))

        self.options.bundletool_version = "1.17.0"
        self.assertNotEqual(base, self._aabkey(ks1))
        self.options.bundletool_version = "1.18.3"

        # resource/dex também invalidam o AAB.
        (self.project / "res" / "values" / "strings.xml").write_text("<resources>x</resources>")
        self.assertNotEqual(base, self._aabkey(ks1))

    def test_single_abi_key_equals_abi_inside_all(self) -> None:
        self._java_source()
        ks = self._keystore("shared.jks", b"key")
        all_ctx = self._context(
            "main", abis=("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
        )
        for name, isolated, inside_all in (
            (
                "resource",
                rc.resource_key(self.project, self.manifest, self.options, self.tc, self.ctx),
                rc.resource_key(self.project, self.manifest, self.options, self.tc, all_ctx),
            ),
            (
                "dex",
                rc.dex_key(self.project, self.manifest, self.options, self.tc, self.ctx),
                rc.dex_key(self.project, self.manifest, self.options, self.tc, all_ctx),
            ),
            (
                "apk",
                rc.apk_key(
                    self.project, self.manifest, self.options, self.tc, self.ctx,
                    apk_root=self._apk_root(), lib_name="main", keystore=ks,
                    abis=("arm64-v8a",),
                ),
                rc.apk_key(
                    self.project, self.manifest, self.options, self.tc, all_ctx,
                    apk_root=self._apk_root(), lib_name="main", keystore=ks,
                    abis=("arm64-v8a",),
                ),
            ),
            ("aab", self._aabkey(ks, abis=("arm64-v8a",)), rc.aab_key(
                self.project, self.manifest, self.options, self.tc, all_ctx,
                apk_root=self._apk_root(), lib_name="main", keystore=ks,
                abis=("arm64-v8a",),
            )),
        ):
            self.assertEqual(isolated, inside_all, name)

        # A seleção de ABI filtra o mapa de libs; não é concatenada na chave.
        kwargs = dict(
            apk_root=self._apk_root(), lib_name="main", keystore=ks
        )
        arm64_key = rc.apk_key(
            self.project, self.manifest, self.options, self.tc, self.ctx,
            abis=("arm64-v8a",), **kwargs,
        )
        x86_key = rc.apk_key(
            self.project, self.manifest, self.options, self.tc, self.ctx,
            abis=("x86",), **kwargs,
        )
        self.assertNotEqual(arm64_key, x86_key)

    def test_dex_blob_hit_miss_and_tamper(self) -> None:
        self._java_source()
        key = self._dkey()
        dex = self.base / "work" / "apk" / "classes.dex"
        dex.parent.mkdir(parents=True, exist_ok=True)
        payload = b"dex\n035\x00" + b"conteudo-do-dex"
        dex.write_bytes(payload)

        digest = rc.store_dex(self.ctx, key, dex)
        self.assertIsNotNone(digest)
        restored = self.base / "restored" / "classes.dex"
        self.assertEqual(rc.load_dex(self.ctx, key, restored), restored)
        self.assertEqual(restored.read_bytes(), payload)

        with self.assertRaises(rc.CacheMiss):
            rc.load_dex(self.ctx, "2" * 64, restored)
        self.ctx.blob_path(digest).write_bytes(b"nao-e-dex")  # type: ignore[arg-type]
        with self.assertRaises(rc.CacheMiss):
            rc.load_dex(self.ctx, key, restored)

    def test_aab_blob_hit_miss_and_tamper(self) -> None:
        ks = self._keystore("app.jks", b"key")
        key = self._aabkey(ks)
        signed = self._zip(self.base / "signed.aab", {"BundleConfig.pb": b"config"})
        digest = rc.store_aab(
            self.ctx,
            key,
            signed,
            metadata=rc.keystore_fingerprint_info(ks),
        )
        self.assertIsNotNone(digest)

        target = self.base / "dist" / "app.aab"
        self.assertEqual(rc.load_aab(self.ctx, key, target), target)
        self.assertEqual(target.read_bytes(), signed.read_bytes())

        with self.assertRaises(rc.CacheMiss):
            rc.load_aab(self.ctx, "3" * 64, target)
        # Keystore diferente = chave diferente = nenhum AAB servido.
        other_ks = self._keystore("other.jks", b"other-key")
        with self.assertRaises(rc.CacheMiss):
            rc.load_aab(self.ctx, self._aabkey(other_ks), target)
        self.ctx.blob_path(digest).write_bytes(b"corrompido")  # type: ignore[arg-type]
        with self.assertRaises(rc.CacheMiss):
            rc.load_aab(self.ctx, key, target)

    def test_bundle_transports_dex_and_aab_blobs(self) -> None:
        self._java_source()
        dex_key = self._dkey()
        dex = self.base / "classes.dex"
        dex.write_bytes(b"dex\n035\x00bundle")
        self.assertIsNotNone(rc.store_dex(self.ctx, dex_key, dex))

        ks = self._keystore("bundle.jks", b"key")
        aab_key = self._aabkey(ks)
        aab = self._zip(self.base / "signed.aab", {"BundleConfig.pb": b"pb"})
        self.assertIsNotNone(rc.store_aab(self.ctx, aab_key, aab))

        bundle = self.base / "bundle.tar.gz"
        manifest = cache_mod.export_bundle(self.ctx, bundle)
        self.assertGreaterEqual(len(manifest["blobs"]), 2)

        other = replace(self.ctx, root=self.base / "other-cache")
        stats = cache_mod.import_bundle(other, bundle)
        self.assertEqual(stats["rejected"], 0)

        restored_dex = rc.load_dex(other, dex_key, self.base / "out" / "classes.dex")
        restored_aab = rc.load_aab(other, aab_key, self.base / "out" / "app.aab")
        self.assertEqual(restored_dex.read_bytes(), dex.read_bytes())
        self.assertEqual(restored_aab.read_bytes(), aab.read_bytes())

    def test_cache_event_shape(self) -> None:
        event = rc.cache_event("apk", "miss", "sem entrada", "abcdef1234567890")
        self.assertEqual(event["stage"], "apk")
        self.assertEqual(event["result"], "miss")
        self.assertEqual(len(event["key"]), 12)
        self.assertEqual(rc.cache_event("link", "qualquer", "x")["result"], "miss")

    def test_build_report_exposes_cache_and_timings(self) -> None:
        options = SimpleNamespace(
            incremental=True,
            cache_ctx=self.ctx,
            cache_namespace="",
            cache_events=[
                rc.cache_event("resource", "hit", "flats reaproveitados", "abc"),
                rc.cache_event("apk", "miss", "keystore diferente", "def"),
            ],
            timings_ms={"toolchain_ms": 5, "total_ms": 42},
            cache_keystore_fingerprint={
                "source": "apkfast.signing.signature_fingerprint",
                "available": True,
                "fingerprint": "abcd",
            },
        )
        args = SimpleNamespace(
            project=str(self.project),
            report="",
            abis="",
            abi="arm64-v8a",
            api=36,
            min_sdk=21,
            target_sdk=36,
            optimize="release",
            sdk_dir=None,
            ndk_dir=None,
            build_tools=None,
            lock_file=None,
            bundletool_version="1.18.3",
        )
        with patch.object(cli, "detect", side_effect=RuntimeError("sem SDK")):
            cli._write_build_report(
                args, self.base / "app.apk", Logger(quiet=True), None, options
            )
        report = json.loads(
            (self.project / "build" / "apkfast-report.json").read_text()
        )
        self.assertEqual(report["cache"]["hits"], 1)
        self.assertEqual(report["cache"]["misses"], 1)
        self.assertEqual(report["cache"]["mode"], "verify")
        self.assertEqual(report["cache"]["events"][1]["reason"], "keystore diferente")
        self.assertEqual(report["timings"]["total_ms"], 42)
        self.assertEqual(report["timings"]["sign_ms"], 0)
        self.assertTrue(report["toolchain"]["error"])


if __name__ == "__main__":
    unittest.main()
