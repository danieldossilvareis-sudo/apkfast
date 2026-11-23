"""Contrato de versão dos headers nativeapp."""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_HPP = ROOT / "include" / "nativeapp" / "version.hpp"
UMBRELLA = ROOT / "include" / "nativeapp" / "nativeapp.hpp"


class VersionHeaderTests(unittest.TestCase):
    def test_macros_and_umbrella_include(self) -> None:
        text = VERSION_HPP.read_text(encoding="utf-8")
        values = {}
        for name in (
            "APKFAST_API_VERSION_MAJOR",
            "APKFAST_API_VERSION_MINOR",
            "APKFAST_API_VERSION_PATCH",
        ):
            match = re.search(rf"#define\s+{name}\s+(\d+)", text)
            self.assertIsNotNone(match, name)
            assert match is not None
            values[name] = int(match.group(1))
        self.assertEqual(
            [values[k] for k in sorted(values)], [2, 4, 0]
        )
        self.assertIn("APKFAST_DEPRECATED", text)
        self.assertIn("source-compatible", text)
        self.assertIn("ABI", text)
        self.assertIn('#include "version.hpp"', UMBRELLA.read_text(encoding="utf-8"))

    @unittest.skipUnless(shutil.which("clang++") or shutil.which("g++"), "sem compilador host")
    def test_header_compiles_standalone(self) -> None:
        compiler = shutil.which("clang++") or shutil.which("g++")
        assert compiler is not None
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "check.cpp"
            source.write_text(
                '#include "version.hpp"\n'
                "static_assert(APKFAST_API_VERSION == 20400, \"versao\");\n"
                "APKFAST_DEPRECATED(\"use novo\") int antigo;\n"
                "int main() { return 0; }\n"
            )
            result = subprocess.run(
                [compiler, "-std=c++17", "-fsyntax-only",
                 f"-I{VERSION_HPP.parent}", str(source)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
