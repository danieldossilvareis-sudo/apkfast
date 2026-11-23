"""Testes da validação de apkfast.json."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from apkfast import config_schema as cs
from apkfast.builder import BuildError, _read_project_config


class ConfigSchemaTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.project = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_current_keys_are_accepted(self) -> None:
        current = {
            "nativeLibraryName": "main",
            "abi": "arm64-v8a",
            "api": 36,
            "minSdk": 21,
            "targetSdk": 36,
            "optimize": "release",
            "obfuscate": False,
            "release": False,
            "keystore": "release.jks",
            "template": "bare",
            "libraries": ["libs/arm64-v8a/libfoo.a"],
            "libsDir": "libs",
            "incremental": True,
            "cleanCache": False,
            "cacheDir": "/tmp/apkfast",
            "cacheMode": "verify",
            "cacheNamespace": "monorepo",
            "requireCacheSignature": True,
        }
        self.assertEqual(cs.validate(current), current)
        (self.project / cs.CONFIG_NAME).write_text(json.dumps(current))
        self.assertEqual(_read_project_config(self.project), current)

    def test_unknown_key_suggests_close_name(self) -> None:
        with self.assertRaisesRegex(cs.ConfigError, "chave desconhecida 'cacheMod'"):
            cs.validate({"cacheMod": "verify"})
        with self.assertRaisesRegex(cs.ConfigError, "'cacheMode'"):
            cs.validate({"cacheMod": "verify"})

    def test_wrong_types_have_clear_messages(self) -> None:
        cases = [
            ({"incremental": "sim"}, "'incremental' deve ser true ou false"),
            ({"api": True}, "'api' deve ser um inteiro positivo"),
            ({"libraries": "libfoo.a"}, "'libraries' deve ser uma lista de textos"),
            ({"cacheMode": " Trusted "}, "'cacheMode' deve ser um de"),
            ({"cacheNamespace": 42}, "'cacheNamespace' deve ser um texto"),
        ]
        for data, message in cases:
            with self.subTest(data=data), self.assertRaisesRegex(cs.ConfigError, message):
                cs.validate(data)

    def test_non_object_and_invalid_json(self) -> None:
        with self.assertRaisesRegex(cs.ConfigError, "objeto JSON"):
            cs.validate([1, 2])
        path = self.project / cs.CONFIG_NAME
        path.write_text("[1, 2]")
        with self.assertRaisesRegex(BuildError, "objeto JSON"):
            _read_project_config(self.project)
        path.write_text("{'x': 1}")
        with self.assertRaisesRegex(cs.ConfigError, "JSON inválido"):
            cs.load(path)

    def test_effective_and_missing_file(self) -> None:
        self.assertEqual(cs.load(self.project / cs.CONFIG_NAME), {})
        effective = cs.effective({"api": 28})
        self.assertEqual(effective["api"], 28)
        self.assertEqual(effective["minSdk"], 21)
        self.assertEqual(effective["abi"], "arm64-v8a")


if __name__ == "__main__":
    unittest.main()
