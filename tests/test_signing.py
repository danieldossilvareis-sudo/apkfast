"""Testes unitários de assinatura, versionCode e pinos de fontes (sem subprocess)."""

from __future__ import annotations

import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from apkfast import raylib, sdl, signing

ALL_HELP = "\n".join(signing._SIGNING_FLAGS.values())


class SigningUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.manifest = self.root / "AndroidManifest.xml"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write_manifest(self, code: int | str | None = 42) -> Path:
        attr = "" if code is None else f' android:versionCode="{code}"'
        self.manifest.write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<manifest xmlns:android="http://schemas.android.com/apk/res/android"'
            f' package="com.example.game"{attr}><application /></manifest>\n',
            encoding="utf-8",
        )
        return self.manifest

    # --- resolve_keystore_options -------------------------------------------------
    def test_cli_overrides_env(self) -> None:
        env = {
            signing.ENV_KEYSTORE: "/env/release.jks",
            signing.ENV_STORE_PASS: "env-store-secret",
            signing.ENV_KEY_PASS: "env-key-secret",
            signing.ENV_KEY_ALIAS: "env-alias",
        }
        opts = signing.resolve_keystore_options(
            keystore=self.root / "cli.jks",
            store_pass="cli-store-secret",
            key_alias="cli-alias",
            env=env,
            release=True,
        )
        self.assertEqual(opts.keystore, self.root / "cli.jks")
        self.assertEqual(opts.store_pass, "cli-store-secret")
        self.assertEqual(opts.key_pass, "env-key-secret")
        self.assertEqual(opts.key_alias, "cli-alias")

    def test_env_fallback_and_debug_defaults(self) -> None:
        env = {
            signing.ENV_KEYSTORE: "/env/release.jks",
            signing.ENV_STORE_PASS: "env-store-secret",
            signing.ENV_KEY_PASS: "env-key-secret",
            signing.ENV_KEY_ALIAS: "env-alias",
        }
        opts = signing.resolve_keystore_options(env=env)
        self.assertEqual(opts.keystore, Path("/env/release.jks"))
        self.assertEqual(opts.store_pass, "env-store-secret")
        self.assertEqual(opts.key_pass, "env-key-secret")
        self.assertEqual(opts.key_alias, "env-alias")

        defaults = signing.resolve_keystore_options(env={})
        self.assertIsNone(defaults.keystore)
        self.assertEqual(defaults.store_pass, signing.DEFAULT_STORE_PASS)
        self.assertEqual(defaults.key_pass, signing.DEFAULT_KEY_PASS)
        self.assertEqual(defaults.key_alias, signing.DEFAULT_KEY_ALIAS)

    def test_release_without_keystore_is_redacted(self) -> None:
        with self.assertRaises(signing.SigningError) as ctx:
            signing.resolve_keystore_options(
                store_pass="super-secreto", env={}, release=True
            )
        message = str(ctx.exception)
        self.assertIn("--keystore", message)
        self.assertIn(signing.ENV_KEYSTORE, message)
        self.assertNotIn("super-secreto", message)

    def test_redaction_of_options_and_dataclass_repr(self) -> None:
        opts = signing.resolve_keystore_options(
            keystore="/tmp/upload.jks",
            store_pass="super-secreto",
            key_pass="outra-senha",
            key_alias="upload",
            env={},
        )
        safe = opts.redacted()
        self.assertEqual(safe["keystore"], "/tmp/upload.jks")
        self.assertEqual(safe["store_pass"], "***")
        self.assertEqual(safe["key_pass"], "***")
        self.assertEqual(safe["key_alias"], "upload")
        self.assertNotIn("super-secreto", repr(opts))
        self.assertNotIn("outra-senha", repr(opts))
        self.assertEqual(signing.redact(None), "<unset>")

    # --- apksigner_sign_args -----------------------------------------------------
    def make_args(self, **kwargs: object) -> list[str]:
        params: dict[str, object] = {
            "keystore": "/tmp/upload.jks",
            "store_pass": "store-secret",
            "key_pass": "key-secret",
            "key_alias": "upload",
            "unsigned": "/tmp/unsigned.apk",
            "output": "/tmp/signed.apk",
            "apksigner": "apksigner",
            "help_text": ALL_HELP,
        }
        params.update(kwargs)
        return signing.apksigner_sign_args(**params)  # type: ignore[arg-type]

    def test_all_schemes_enabled_by_default(self) -> None:
        cmd = self.make_args()
        self.assertEqual(cmd[:2], ["apksigner", "sign"])
        self.assertEqual(cmd[cmd.index("--ks") + 1], "/tmp/upload.jks")
        self.assertEqual(cmd[cmd.index("--ks-pass") + 1], "pass:store-secret")
        self.assertEqual(cmd[cmd.index("--key-pass") + 1], "pass:key-secret")
        self.assertEqual(cmd[cmd.index("--ks-key-alias") + 1], "upload")
        for flag in signing._SIGNING_FLAGS.values():
            self.assertEqual(cmd[cmd.index(flag) + 1], "true", flag)
        self.assertEqual(cmd[-3:], ["--out", "/tmp/signed.apk", "/tmp/unsigned.apk"])

    def test_disable_flags_and_redact_command(self) -> None:
        cmd = self.make_args(v1=False, v3=False)
        for name, flag in signing._SIGNING_FLAGS.items():
            expected = "false" if name in ("v1", "v3") else "true"
            self.assertEqual(cmd[cmd.index(flag) + 1], expected, name)
        safe = signing.redact_args(cmd)
        self.assertNotIn("store-secret", safe)
        self.assertNotIn("key-secret", safe)
        self.assertIn("***", safe)
        self.assertEqual(safe[safe.index("--ks-pass") + 1], "***")
        eq_form = signing.redact_args(["--ks-pass=pass:abc", "--key-pass=pass:def"])
        self.assertEqual(eq_form, ["--ks-pass=***", "--key-pass=***"])

    def test_degrades_with_warning_when_help_lacks_flags(self) -> None:
        warns: list[str] = []
        cmd = self.make_args(
            help_text="--v1-signing-enabled --v2-signing-enabled",
            warn=warns.append,
        )
        self.assertIn("--v1-signing-enabled", cmd)
        self.assertIn("--v2-signing-enabled", cmd)
        self.assertNotIn("--v3-signing-enabled", cmd)
        self.assertNotIn("--v4-signing-enabled", cmd)
        self.assertEqual(len(warns), 2)
        self.assertTrue(all("store-secret" not in w for w in warns))
        self.assertTrue(any("--v3-signing-enabled" in w for w in warns))

    def test_probe_failure_degrades_v3_v4(self) -> None:
        warns: list[str] = []
        with patch.object(
            signing, "apksigner_help", side_effect=signing.SigningError("sem help")
        ) as probe:
            cmd = signing.apksigner_sign_args(
                keystore="/tmp/upload.jks",
                store_pass="store-secret",
                key_pass="key-secret",
                key_alias="upload",
                unsigned="/tmp/in.apk",
                output="/tmp/out.apk",
                apksigner="/opt/apksigner",
                warn=warns.append,
            )
        probe.assert_called_once()
        self.assertIn("--v2-signing-enabled", cmd)
        self.assertNotIn("--v3-signing-enabled", cmd)
        self.assertNotIn("--v4-signing-enabled", cmd)
        self.assertTrue(warns)

    def test_help_text_skips_probe(self) -> None:
        with patch.object(signing, "apksigner_help", side_effect=AssertionError):
            self.make_args(help_text=ALL_HELP)

    def test_requires_keystore_and_alias(self) -> None:
        with self.assertRaisesRegex(signing.SigningError, "Keystore"):
            self.make_args(keystore="")
        with self.assertRaisesRegex(signing.SigningError, "Alias"):
            self.make_args(key_alias="")

    # --- next_version_code -------------------------------------------------------
    def test_manifest_plus_one(self) -> None:
        self.assertEqual(
            signing.next_version_code(self.write_manifest(42)), 43
        )
        self.assertEqual(
            signing.next_version_code(self.write_manifest("7"), strategy="manifest+1"),
            8,
        )

    def test_manifest_without_version_code_or_invalid_xml(self) -> None:
        self.write_manifest(None)
        with self.assertRaisesRegex(signing.SigningError, "versionCode"):
            signing.next_version_code(self.manifest)
        self.manifest.write_text("<manifest", encoding="utf-8")
        with self.assertRaisesRegex(signing.SigningError, "inválido"):
            signing.next_version_code(self.manifest)

    def test_explicit_strategy(self) -> None:
        self.write_manifest(1)
        self.assertEqual(
            signing.next_version_code(self.manifest, "explicit", explicit="77"), 77
        )
        with self.assertRaisesRegex(signing.SigningError, "explicit"):
            signing.next_version_code(self.manifest, "explicit")
        with self.assertRaisesRegex(signing.SigningError, "inteiro"):
            signing.next_version_code(self.manifest, "explicit", explicit="abc")
        with self.assertRaisesRegex(signing.SigningError, "fora do intervalo"):
            signing.next_version_code(
                self.manifest, "explicit", explicit=signing.MAX_VERSION_CODE + 1
            )

    def test_timestamp_strategy_uses_seconds_since_2020(self) -> None:
        self.write_manifest(1)
        self.assertEqual(
            signing.next_version_code(
                self.manifest, "timestamp", now_ms=1_800_000_000_123
            ),
            222_163_200,
        )
        # Alias legado não precisa ler versionCode do manifesto.
        self.write_manifest(None)
        self.assertEqual(
            signing.next_version_code(self.manifest, timestamp=True, now_ms=1_700_000_000_999),
            122_163_200,
        )
        with self.assertRaisesRegex(signing.SigningError, "anterior a 2020"):
            signing.next_version_code(self.manifest, "timestamp", now_ms=-1)
        with self.assertRaisesRegex(signing.SigningError, "desconhecida"):
            signing.next_version_code(self.manifest, "build-number")

    def test_timestamp_stays_inside_play_range(self) -> None:
        self.write_manifest(1)
        base = signing.TIMESTAMP_BASE_MS
        for now_ms, expected in (
            (base, 1),
            (base + 999, 1),
            (base + 1_000, 1),
            (base + 1_999, 1),
            (base + 2_000, 2),
            (base + signing.MAX_VERSION_CODE * 1_000, signing.MAX_VERSION_CODE),
        ):
            with self.subTest(now_ms=now_ms):
                value = signing.next_version_code(
                    self.manifest, "timestamp", now_ms=now_ms
                )
                self.assertTrue(1 <= value <= signing.MAX_VERSION_CODE)
                self.assertEqual(value, expected)

    def test_timestamp_is_monotonic_per_second(self) -> None:
        self.write_manifest(1)
        now_ms = signing.TIMESTAMP_BASE_MS + 1_000
        previous = signing.next_version_code(self.manifest, "timestamp", now_ms=now_ms)
        for _ in range(10):
            for sub_second in (250, 500, 999):
                self.assertEqual(
                    signing.next_version_code(
                        self.manifest, "timestamp", now_ms=now_ms + sub_second
                    ),
                    previous,
                )
            now_ms += 1_000
            current = signing.next_version_code(
                self.manifest, "timestamp", now_ms=now_ms
            )
            self.assertEqual(current, previous + 1)
            previous = current

    def test_timestamp_with_injected_now_is_deterministic(self) -> None:
        self.write_manifest(1)
        now_ms = signing.TIMESTAMP_BASE_MS + 123_456_789_123
        with patch.object(
            signing.time, "time", side_effect=AssertionError("relógio não deve ser lido")
        ):
            first = signing.next_version_code(
                self.manifest, "timestamp", now_ms=now_ms
            )
            second = signing.next_version_code(
                self.manifest, "timestamp", now_ms=now_ms
            )
        self.assertEqual(first, second)
        self.assertEqual(first, 123_456_789)

    def test_timestamp_survives_old_epoch_limit(self) -> None:
        self.write_manifest(1)
        # Em 2038 o esquema antigo (epoch/1000) já estouraria 2.1e9.
        now_ms = 2_160_000_000_123
        self.assertGreater(now_ms // 1_000, signing.MAX_VERSION_CODE)
        value = signing.next_version_code(self.manifest, "timestamp", now_ms=now_ms)
        self.assertEqual(value, 582_163_200)
        self.assertTrue(1 <= value <= signing.MAX_VERSION_CODE)
        self.assertGreater(
            signing.next_version_code(
                self.manifest, "timestamp", now_ms=now_ms + 1_000
            ),
            value,
        )

    def test_timestamp_rejects_beyond_play_limit(self) -> None:
        self.write_manifest(1)
        last_ok_ms = (
            signing.TIMESTAMP_BASE_MS + signing.MAX_VERSION_CODE * 1_000
        )
        self.assertEqual(
            signing.next_version_code(self.manifest, "timestamp", now_ms=last_ok_ms),
            signing.MAX_VERSION_CODE,
        )
        # Ainda dentro do mesmo segundo: truncamento não altera o valor.
        self.assertEqual(
            signing.next_version_code(
                self.manifest, "timestamp", now_ms=last_ok_ms + 999
            ),
            signing.MAX_VERSION_CODE,
        )
        with self.assertRaisesRegex(signing.SigningError, "limite"):
            signing.next_version_code(
                self.manifest, "timestamp", now_ms=last_ok_ms + 1_000
            )

    # --- signature_fingerprint ---------------------------------------------------
    def test_fingerprint_is_sha256_of_keystore_file(self) -> None:
        keystore = self.root / "upload.jks"
        keystore.write_bytes(b"keystore-bytes")
        expected = hashlib.sha256(b"keystore-bytes").hexdigest()
        self.assertEqual(signing.signature_fingerprint(keystore), expected)
        with self.assertRaisesRegex(signing.SigningError, "não encontrada"):
            signing.signature_fingerprint(self.root / "missing.jks")


class SourcePinTests(unittest.TestCase):
    """Pinos e validação de override das fontes (sem download)."""

    def test_default_pins_are_well_formed(self) -> None:
        for name, version, sha in (
            ("raylib", raylib.RAYLIB_VERSION, raylib.RAYLIB_SHA256),
            ("SDL2", sdl.SDL2_VERSION, sdl.SDL2_SHA256),
            ("SDL3", sdl.SDL3_VERSION, sdl.SDL3_SHA256),
        ):
            with self.subTest(name=name):
                self.assertRegex(version, r"^\d+\.\d+(?:\.\d+)?$")
                self.assertRegex(sha, r"^[0-9a-f]{64}$")

    def test_alternate_version_requires_sha256(self) -> None:
        with self.assertRaisesRegex(raylib.RaylibError, "APKFAST_RAYLIB_SHA256"):
            raylib._resolve_pin(env={"APKFAST_RAYLIB_VERSION": "5.4"})
        with self.assertRaisesRegex(sdl.SDLError, "APKFAST_SDL3_SHA256"):
            sdl._resolve_pin(3, env={"APKFAST_SDL3_VERSION": "3.4.19"})

    def test_env_override_is_accepted_with_sha(self) -> None:
        digest = "a" * 64
        version, sha, url = raylib._resolve_pin(
            env={"APKFAST_RAYLIB_VERSION": "5.4", "APKFAST_RAYLIB_SHA256": digest}
        )
        self.assertEqual((version, sha), ("5.4", digest))
        self.assertIn("5.4.tar.gz", url)
        version, sha, url = sdl._resolve_pin(
            2,
            env={"APKFAST_SDL2_VERSION": "2.32.8", "APKFAST_SDL2_SHA256": digest},
        )
        self.assertEqual((version, sha), ("2.32.8", digest))
        self.assertIn("SDL2-2.32.8.tar.gz", url)

    def test_sha_mismatch_has_clear_message(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad = root / "bad.bin"
            bad.write_bytes(b"conteudo-divergente")
            with self.assertRaises(raylib.RaylibError) as ctx:
                raylib._download(
                    bad.as_uri(), root / "bad.tar.gz", "0" * 64, "5.5", quiet=True
                )
            message = str(ctx.exception)
            self.assertIn("divergiu", message)
            self.assertIn("0" * 64, message)
            self.assertIn("APKFAST_RAYLIB_SHA256", message)

    def test_symlink_traversal_is_rejected(self) -> None:
        top = "SDL2-2.32.10"
        self.assertTrue(sdl._safe_link(f"{top}/android-project-ant/src", "../app/src", top))
        for link in ("../../fora", "/etc/passwd", ""):
            with self.subTest(link=link):
                self.assertFalse(
                    sdl._safe_link(f"{top}/android-project-ant/src", link, top)
                )

    def test_inconsistent_marker_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)
            (source / ".apkfast-source.json").write_text(
                json.dumps({"version": "5.0", "sha256": "b" * 64}), encoding="utf-8"
            )
            with self.assertRaisesRegex(raylib.RaylibError, "Remova o diretório"):
                raylib._check_pin_marker(
                    source, raylib.RAYLIB_VERSION, raylib.RAYLIB_SHA256
                )


if __name__ == "__main__":
    unittest.main()
