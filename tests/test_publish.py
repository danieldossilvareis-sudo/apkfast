"""Testes unitários de apkfast.publish (subprocess mockado, sem rede)."""

from __future__ import annotations

import argparse
import importlib.metadata
import io
import os
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

from apkfast import publish


def _done(code: int = 0, out: str = "", err: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess(args=[], returncode=code, stdout=out, stderr=err)


def _artifacts(out_dir: Path) -> tuple[Path, Path]:
    whl = out_dir / "apkfast-2.4.1-py3-none-any.whl"
    gz = out_dir / "apkfast-2.4.1.tar.gz"
    whl.write_bytes(b"wheel")
    gz.write_bytes(b"sdist")
    return whl, gz


class PublishParserTests(unittest.TestCase):
    def setUp(self) -> None:
        self.parser = argparse.ArgumentParser(prog="apkfast")
        publish.register(self.parser.add_subparsers(dest="command"))

    def test_commands_registered(self) -> None:
        dist = self.parser.parse_args(["dist"])
        self.assertEqual(dist.command, "dist")
        self.assertTrue(callable(dist.func))
        self.assertEqual(dist.out_dir, "dist")

        pub = self.parser.parse_args(["publish-pypi", "--repository", "testpypi"])
        self.assertEqual(pub.command, "publish-pypi")
        self.assertEqual(pub.repository, "testpypi")
        self.assertTrue(pub.dry_run, "dry-run deve ser o padrão")
        self.assertTrue(callable(pub.func))

        real = self.parser.parse_args(["publish-pypi", "--yes"])
        self.assertFalse(real.dry_run)

    def test_dry_run_and_yes_are_exclusive(self) -> None:
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                self.parser.parse_args(["publish-pypi", "--dry-run", "--yes"])


class BuildDistTests(unittest.TestCase):
    def test_uses_python_m_build_when_available(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            out = Path(tmp) / "dist"
            root.mkdir()

            def fake_run(cmd: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                out.mkdir(exist_ok=True)
                _artifacts(out)
                return _done()

            with (
                patch("apkfast.publish.importlib.metadata.version", return_value="1.2.3"),
                patch("apkfast.publish.subprocess.run", side_effect=fake_run) as run,
            ):
                result = publish.build_dist(root, out)

            cmd = run.call_args.args[0]
            self.assertEqual(cmd[1:3], ["-m", "build"])
            self.assertIn("--sdist", cmd)
            self.assertIn("--wheel", cmd)
            self.assertEqual(result.backend, "python -m build")
            self.assertEqual(result.count, 2)
            self.assertEqual({p.suffix for p in result.files}, {".whl", ".gz"})
            self.assertIn("2 artefato(s)", result.summary())

    def test_falls_back_to_setup_py(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            out = Path(tmp) / "dist"
            root.mkdir()
            (root / "setup.py").write_text("# legado\n")

            def fake_run(cmd: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                out.mkdir(exist_ok=True)
                _artifacts(out)
                return _done()

            missing = importlib.metadata.PackageNotFoundError("build")
            with (
                patch("apkfast.publish.importlib.metadata.version", side_effect=missing),
                patch("apkfast.publish.subprocess.run", side_effect=fake_run) as run,
            ):
                result = publish.build_dist(root, out)

            cmd = run.call_args.args[0]
            self.assertIn("setup.py", cmd)
            self.assertIn("sdist", cmd)
            self.assertIn("bdist_wheel", cmd)
            self.assertEqual(result.backend, "setup.py sdist bdist_wheel")

    def test_failure_keeps_tail_and_never_invents_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "repo"
            out = Path(tmp) / "dist"
            root.mkdir()
            with (
                patch("apkfast.publish.importlib.metadata.version", return_value="1.2.3"),
                patch(
                    "apkfast.publish.subprocess.run",
                    return_value=_done(1, err="ERROR: metadata quebrado"),
                ),
            ):
                with self.assertRaisesRegex(publish.PublishError, "metadata quebrado"):
                    publish.build_dist(root, out)


class CheckDistTests(unittest.TestCase):
    def test_runs_twine_check(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            whl, gz = _artifacts(out)
            with (
                patch("apkfast.publish.shutil.which", return_value="/usr/bin/twine"),
                patch("apkfast.publish.subprocess.run", return_value=_done()) as run,
            ):
                result = publish.check_dist([whl, gz])

            cmd = run.call_args.args[0]
            self.assertEqual(cmd[:2], ["/usr/bin/twine", "check"])
            self.assertIn(str(whl), cmd)
            self.assertIn(str(gz), cmd)
            self.assertEqual(result.count, 2)
            self.assertIn("2 artefato(s)", result.summary())


class UploadTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.whl, self.gz = _artifacts(Path(self.tmp.name))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_dry_run_never_calls_subprocess(self) -> None:
        secret = "pypi-SEGREDOdryrun123"
        env = {"PYPI_TOKEN": secret}
        with (
            patch.dict(os.environ, env, clear=True),
            patch("apkfast.publish.subprocess.run") as run,
        ):
            result = publish.upload([self.whl, self.gz], repository="testpypi", dry_run=True)

        run.assert_not_called()
        self.assertEqual(result.mode, "dry-run")
        self.assertEqual(result.repository, "TestPyPI")
        self.assertIn("--repository-url", result.command)
        self.assertIn("https://test.pypi.org/legacy/", result.command)
        self.assertNotIn(secret, result.summary())

    def test_real_upload_sends_token_only_via_environment(self) -> None:
        secret = "pypi-ABCDEF1234567890"
        with (
            patch.dict(os.environ, {"PYPI_TOKEN": secret}, clear=True),
            patch("apkfast.publish.shutil.which", return_value="/usr/bin/twine"),
            patch("apkfast.publish.subprocess.run", return_value=_done()) as run,
        ):
            result = publish.upload([self.whl], dry_run=False, yes=True)

        cmd = run.call_args.args[0]
        child_env = run.call_args.kwargs["env"]
        self.assertEqual(cmd[:2], ["/usr/bin/twine", "upload"])
        self.assertEqual(child_env["TWINE_USERNAME"], "__token__")
        self.assertEqual(child_env["TWINE_PASSWORD"], secret)
        self.assertNotIn("PYPI_TOKEN", child_env)
        self.assertNotIn("TWINE_API_TOKEN", child_env)
        self.assertNotIn(secret, " ".join(cmd))
        self.assertNotIn(secret, result.summary())
        self.assertEqual(result.mode, "upload")

    def test_requires_yes_and_then_token(self) -> None:
        with (
            patch.dict(os.environ, {}, clear=True),
            patch("apkfast.publish.subprocess.run") as run,
        ):
            with self.assertRaisesRegex(publish.PublishError, "--yes"):
                publish.upload([self.whl], dry_run=False, yes=False)
            with self.assertRaisesRegex(publish.PublishError, "TWINE_"):
                publish.upload([self.whl], dry_run=False, yes=True)
        run.assert_not_called()

    def test_error_output_is_redacted(self) -> None:
        secret = "pypi-SEGREDO123456789"
        with patch(
            "apkfast.publish.subprocess.run",
            return_value=_done(1, err=f"invalid password: {secret}"),
        ):
            with self.assertRaises(publish.PublishError) as caught:
                publish.upload([self.whl], dry_run=False, yes=True, env={"PYPI_TOKEN": secret})
        self.assertNotIn(secret, str(caught.exception))
        self.assertIn("***", str(caught.exception))


class PublishCommandTests(unittest.TestCase):
    def setUp(self) -> None:
        self.parser = argparse.ArgumentParser(prog="apkfast")
        publish.register(self.parser.add_subparsers(dest="command"))
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / "dist"
        self.out.mkdir()
        _artifacts(self.out)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_publish_pypi_defaults_to_dry_run_and_hides_token(self) -> None:
        secret = "pypi-TOKENvisivelNAO123"
        args = self.parser.parse_args(
            ["publish-pypi", "--skip-build", "--out-dir", str(self.out)]
        )
        with (
            patch.dict(os.environ, {"PYPI_TOKEN": secret}, clear=True),
            patch("apkfast.publish.subprocess.run", return_value=_done()) as run,
        ):
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = publish._cmd_publish_pypi(args)

        self.assertEqual(code, 0)
        self.assertEqual(run.call_count, 1, "somente twine check deve rodar")
        self.assertIn("check", run.call_args.args[0])
        printed = stdout.getvalue()
        self.assertIn("DRY-RUN", printed)
        self.assertIn("modo=dry-run", printed)
        self.assertNotIn(secret, printed)

    def test_dist_command_prints_counts(self) -> None:
        args = self.parser.parse_args(["dist", "--out-dir", str(self.out)])
        built = publish.DistResult(
            root=str(Path.cwd()), out_dir=str(self.out), backend="python -m build",
            command="python -m build", files=tuple(sorted(self.out.iterdir())),
        )
        checked = publish.CheckResult(checked=built.files, command="twine check")
        with (
            patch.object(publish, "build_dist", return_value=built),
            patch.object(publish, "check_dist", return_value=checked),
        ):
            stdout = io.StringIO()
            with redirect_stdout(stdout):
                code = publish._cmd_dist(args)
        self.assertEqual(code, 0)
        self.assertIn("2 artefato(s)", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()
