"""Testes dos scripts de conclusão e dos novos comandos do CLI."""

from __future__ import annotations

import io
import json
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from apkfast import cli, completions, lockfile as lf
from apkfast.builder import Logger


class CompletionTests(unittest.TestCase):
    def test_all_shells(self) -> None:
        names, subs = completions.commands()
        for command in ("build", "init", "doctor", "config", "completions"):
            self.assertIn(command, names)
        self.assertIn("--generate-lock", subs["build"])
        self.assertIn("--report", subs["build"])
        for shell in completions.SHELLS:
            script = completions.generate(shell)
            self.assertIn("build", script)
            self.assertIn("config", script)
            if shell == "bash":
                self.assertIn("complete -F _apkfast apkfast", script)
            elif shell == "zsh":
                self.assertIn("#compdef apkfast", script)
            else:
                self.assertIn("complete -c apkfast -f", script)

    def test_unsupported_shell(self) -> None:
        with self.assertRaises(ValueError):
            completions.generate("powershell")

    def test_cli_flag_and_subcommand(self) -> None:
        for argv in (["--completions", "bash"], ["completions", "bash"]):
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(cli.main(argv), 0)
            self.assertIn("_apkfast", out.getvalue())


class CliAdditionsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.project = Path(self.tmp.name)
        (self.project / "apkfast.json").write_text(
            '{"incremental": true, "cacheMode": "verify"}\n'
        )
        self.tc = SimpleNamespace(
            sdk_dir=Path("/sdk/android-sdk"),
            ndk_dir=Path("/sdk/android-ndk-r30"),
            build_tools_dir=Path("/sdk/build-tools/36.0.0"),
            platform_dir=Path("/sdk/platforms/android-36"),
            api=36,
            abi="arm64-v8a",
            host_tag="linux-arm64",
        )

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_config_show(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(cli.main(["config", "show", str(self.project)]), 0)
        payload = json.loads(out.getvalue())
        self.assertEqual(payload["config"]["cacheMode"], "verify")
        self.assertEqual(payload["effective"]["minSdk"], 21)
        self.assertTrue(payload["present"])

    def test_generate_lock_from_cli(self) -> None:
        with patch.object(cli, "detect", return_value=self.tc):
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(
                    cli.main(["build", str(self.project), "--generate-lock"]), 0
                )
        lock = lf.load(self.project)
        assert lock is not None
        self.assertEqual(lock["buildTools"], "36.0.0")
        self.assertIn("Lockfile gerado", out.getvalue())

    def test_doctor_json_without_toolchain(self) -> None:
        with patch.object(cli, "detect", side_effect=RuntimeError("sem SDK")):
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(
                    cli.main(["doctor", "--json", "--project", str(self.project)]), 1
                )
        payload = json.loads(out.getvalue())
        self.assertFalse(payload["ok"])
        self.assertEqual(payload["error"], "sem SDK")
        self.assertEqual(payload["lock"]["status"], "missing")
        self.assertTrue(payload["config"]["valid"])

    def test_build_report_helper(self) -> None:
        apk = self.project / "dist" / "app.apk"
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
        )
        with patch.object(cli, "detect", return_value=self.tc):
            cli._write_build_report(args, apk, Logger(quiet=True))
        report = json.loads(
            (self.project / "build" / "apkfast-report.json").read_text()
        )
        self.assertEqual(report["apiVersion"], "2.4.0")
        self.assertEqual(report["output"], str(apk.resolve()))
        self.assertEqual(report["toolchain"]["buildTools"], "36.0.0")
        self.assertTrue(report["configValid"])


if __name__ == "__main__":
    unittest.main()
