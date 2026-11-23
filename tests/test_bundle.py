"""Testes unitários do suporte a AAB (sem download do bundletool)."""

from __future__ import annotations

import hashlib
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from apkfast import bundle
from apkfast.builder import _unpack_compiled_flat

MANIFEST_PB = b"\x0a\x06manifest-proto"
RESOURCES_PB = b"\x08\x01resources-proto"


class BundleUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_pinned_version_and_digest(self) -> None:
        self.assertEqual(bundle.BUNDLETOOL_VERSION, "1.18.3")
        self.assertRegex(bundle.BUNDLETOOL_SHA256, r"^[0-9a-f]{64}$")
        self.assertEqual(bundle.asset_name("1.18.3"), "bundletool-all-1.18.3.jar")
        self.assertEqual(
            bundle.bundletool_url("1.18.3"),
            "https://github.com/google/bundletool/releases/download/"
            "1.18.3/bundletool-all-1.18.3.jar",
        )

    def test_validate_version_rejects_traversal(self) -> None:
        for value in ("", "1.2", "1.2.3/../../x", "../1.2.3", "v1.2.3"):
            with self.subTest(value=value):
                with self.assertRaises(bundle.BundleError):
                    bundle.validate_version(value)

    def test_parse_version(self) -> None:
        self.assertEqual(bundle.parse_version("bundletool 1.18.3\n"), "1.18.3")
        with self.assertRaises(bundle.BundleError):
            bundle.parse_version("sem versão")

    def test_clear_errors_for_missing_tools(self) -> None:
        with patch.object(bundle.shutil, "which", return_value=None):
            with self.assertRaisesRegex(bundle.BundleError, "jarsigner não encontrado"):
                bundle.require_jarsigner()
            with self.assertRaisesRegex(bundle.BundleError, "Java não encontrado"):
                bundle.require_java()
        with self.assertRaisesRegex(bundle.BundleError, "bundletool não encontrado"):
            bundle._bundletool_command(self.root / "missing.jar")

    def test_default_aab_path_mirrors_apk(self) -> None:
        dist = self.root / "dist"
        self.assertEqual(
            bundle.default_aab_path("com.foo.bar", "arm64-v8a", dist),
            dist / "bar-arm64-v8a.aab",
        )
        self.assertEqual(
            bundle.default_aab_path(
                "com.foo.bar", "all-release", dist,
                apk_output=self.root / "custom.apk",
            ),
            self.root / "custom.aab",
        )
        self.assertEqual(
            bundle.default_aab_path(
                "com.foo.bar", "x86", dist, explicit=self.root / "upload.aab"
            ),
            self.root / "upload.aab",
        )

    def test_ensure_bundletool_uses_verified_cache(self) -> None:
        jar = self.root / "bundletool-all-1.18.3.jar"
        jar.write_bytes(b"jar-fake")
        digest = hashlib.sha256(b"jar-fake").hexdigest()
        with patch.object(bundle, "_expected_sha256", return_value=digest), patch.object(
            bundle, "_run_command", side_effect=AssertionError("download indevido")
        ):
            self.assertEqual(
                bundle.ensure_bundletool(quiet=True, directory=self.root), jar
            )

    def _proto_apk(self) -> Path:
        path = self.root / "proto.apk"
        with zipfile.ZipFile(path, "w") as archive:
            archive.writestr("AndroidManifest.xml", MANIFEST_PB)
            archive.writestr("resources.pb", RESOURCES_PB)
            archive.writestr("res/values/values.pb", b"res-proto")
            # Presente no APK proto, mas não deve duplicar --assets.
            archive.writestr("assets/from-proto.bin", b"ignored")
        return path

    def test_prepare_module_layout(self) -> None:
        dex = self.root / "apk"
        dex.mkdir()
        (dex / "classes.dex").write_bytes(b"dex1")
        (dex / "classes2.dex").write_bytes(b"dex2")
        (dex / "classes99.dex").write_bytes(b"dex99")
        lib = dex / "lib" / "arm64-v8a"
        lib.mkdir(parents=True)
        (lib / "libmain.so").write_bytes(b"so")
        assets = self.root / "assets"
        assets.mkdir()
        (assets / "sprite.png").write_bytes(b"png")

        base = bundle.prepare_module(
            work=self.root / "work",
            proto_apk=self._proto_apk(),
            dex_dir=dex,
            lib_dir=dex / "lib",
            assets_dir=assets,
        )
        with zipfile.ZipFile(base) as archive:
            names = set(archive.namelist())
            self.assertEqual(
                names,
                {
                    "manifest/AndroidManifest.xml",
                    "resources.pb",
                    "res/values/values.pb",
                    "dex/classes.dex",
                    "dex/classes2.dex",
                    "dex/classes99.dex",
                    "lib/arm64-v8a/libmain.so",
                    "assets/sprite.png",
                },
            )
            self.assertEqual(archive.read("manifest/AndroidManifest.xml"), MANIFEST_PB)
            self.assertEqual(archive.read("resources.pb"), RESOURCES_PB)

    def test_prepare_module_rejects_bad_abi(self) -> None:
        lib = self.root / "lib"
        (lib / "mips").mkdir(parents=True)
        (lib / "mips" / "libx.so").write_bytes(b"x")
        with self.assertRaisesRegex(bundle.BundleError, "lib/<abi>"):
            bundle.prepare_module(
                work=self.root / "work",
                proto_apk=self._proto_apk(),
                lib_dir=lib,
            )

    def test_build_aab_invokes_bundletool(self) -> None:
        jar = self.root / "bundletool.jar"
        jar.write_bytes(b"fake")

        def fake_run(_bundletool: Path, args: list[str], **_kwargs: object):
            output = next(a.split("=", 1)[1] for a in args if a.startswith("--output="))
            Path(output).write_bytes(b"aab")
            return subprocess.CompletedProcess(args, 0)

        with patch.object(bundle, "run_bundletool", side_effect=fake_run) as runner:
            aab = bundle.build_aab(
                work=self.root / "work",
                proto_apk=self._proto_apk(),
                bundletool=jar,
                quiet=True,
            )
        self.assertEqual(aab, self.root / "work" / "aab" / "unsigned.aab")
        args = runner.call_args.args[1]
        self.assertEqual(args[0], "build-bundle")
        self.assertTrue(any(a.startswith("--modules=") and a.endswith("base.zip") for a in args))

    def test_sign_aab_uses_jarsigner(self) -> None:
        aab = self.root / "app.aab"
        aab.write_bytes(b"unsigned")
        keystore = self.root / "key.jks"
        keystore.write_bytes(b"keystore")
        signed = self.root / "signed.aab"

        with patch.object(bundle, "require_jarsigner", return_value=Path("/bin/jarsigner")), \
             patch.object(bundle, "require_java", return_value=Path("/bin/java")), \
             patch.object(bundle, "_run_command", return_value=subprocess.CompletedProcess([], 0, "jar signed.")) as run:
            result = bundle.sign_aab(
                aab,
                keystore=keystore,
                store_password="store",
                key_alias="upload",
                key_password="key",
                output=signed,
                quiet=True,
            )
        self.assertEqual(result, signed)
        cmd = [str(c) for c in run.call_args_list[0].args[0]]
        self.assertEqual(cmd[0], "/bin/jarsigner")
        self.assertIn("-signedjar", cmd)
        self.assertIn("SHA-256", cmd)
        self.assertIn("upload", cmd)
        self.assertEqual(cmd[-1], "upload")

    def test_unpack_compiled_flat_from_zip_container(self) -> None:
        compiled = self.root / "compiled"
        compiled.mkdir()
        outer = compiled / "values_strings.xml.flat"
        with zipfile.ZipFile(outer, "w") as archive:
            archive.writestr("values_strings.arsc.flat", b"flat-real")
        result = _unpack_compiled_flat(outer, compiled)
        self.assertEqual(result, [compiled / "values_strings.arsc.flat"])
        self.assertEqual(result[0].read_bytes(), b"flat-real")
        self.assertFalse(outer.exists())

        plain = compiled / "drawable_x.png.flat"
        plain.write_bytes(b"flat-direto")
        self.assertEqual(_unpack_compiled_flat(plain, compiled), [plain])

    def test_verify_aab_checks_manifest(self) -> None:
        def fake_run(_bundletool: Path, args: list[str], **_kwargs: object):
            output = "<manifest package=\"com.example.app\"></manifest>"
            return subprocess.CompletedProcess(args, 0, output)

        jar = self.root / "bundletool.jar"
        jar.write_bytes(b"fake")
        with patch.object(bundle, "run_bundletool", side_effect=fake_run):
            result = bundle.verify_aab(
                self._proto_apk(), bundletool=jar, build_apks=False, quiet=True
            )
        self.assertIn("com.example.app", result.manifest_xml)
        self.assertIsNone(result.apks_path)


if __name__ == "__main__":
    unittest.main()
