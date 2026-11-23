"""Geração e publicação da distribuição Python do apkfast.

Segurança primeiro:

- ``publish-pypi`` é **dry-run por padrão**; ``--yes`` é obrigatório para
  qualquer upload real;
- o token nunca vai na linha de comando: ele é passado ao twine pelas
  variáveis ``TWINE_USERNAME``/``TWINE_PASSWORD``;
- qualquer saída de subprocess é redigida antes de virar mensagem de erro;
- o plano exibido mostra apenas o comando, nunca o valor das credenciais.

Este módulo não publica nada sozinho quando importado. O comando real só
acontece após ``--yes`` e com token explícito no ambiente.
"""

from __future__ import annotations

import argparse
import importlib.metadata
import os
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence

__all__ = [
    "PublishError",
    "DistResult",
    "CheckResult",
    "UploadResult",
    "build_dist",
    "check_dist",
    "upload",
    "register",
]

_TESTPYPI_URL = "https://test.pypi.org/legacy/"
_GENERIC_TOKEN = re.compile(r"pypi-[A-Za-z0-9_-]{10,}")


class PublishError(RuntimeError):
    """Erro amigável de build/checagem/upload."""


def _redact(text: str, secrets: Iterable[str]) -> str:
    """Remove tokens conhecidos e tokens PyPI genéricos de uma mensagem."""
    result = text or ""
    for secret in secrets:
        if secret:
            result = result.replace(secret, "***")
    return _GENERIC_TOKEN.sub("***", result)


def _display(cmd: Sequence[str]) -> str:
    """Comando legível e seguro para mostrar ao usuário."""
    return shlex.join(str(part) for part in cmd)


def _tool_prefix(name: str) -> list[str]:
    """Usa o executável quando existe; senão ``python -m <name>``."""
    found = shutil.which(name)
    return [found] if found else [sys.executable, "-m", name]


def _run(
    cmd: Sequence[str],
    *,
    cwd: Path | None = None,
    env: Mapping[str, str] | None = None,
    secrets: Iterable[str] = (),
    what: str = "comando",
) -> str:
    """Executa um comando sem imprimir segredos e devolve a saída combinada."""
    try:
        proc = subprocess.run(
            [str(part) for part in cmd],
            cwd=str(cwd) if cwd is not None else None,
            env=dict(env) if env is not None else None,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        raise PublishError(f"Falha ao executar {what}: {_redact(str(exc), secrets)}") from exc
    except subprocess.SubprocessError as exc:  # defensivo
        raise PublishError(f"Falha ao executar {what}: {_redact(str(exc), secrets)}") from exc

    output = f"{proc.stdout or ''}{proc.stderr or ''}".strip()
    if proc.returncode != 0:
        tail = " | ".join(output.splitlines()[-5:]) if output else "sem saída"
        raise PublishError(f"{what} falhou ({proc.returncode}): {_redact(tail, secrets)}")
    return _redact(output, secrets)


def _has_build_module() -> bool:
    """Detecta o *distribuído* ``build`` (não um diretório ``build/`` local)."""
    try:
        importlib.metadata.version("build")
    except importlib.metadata.PackageNotFoundError:
        return False
    return True


def _as_file(value: str | Path) -> Path:
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        raise PublishError(f"Arquivo de distribuição não encontrado: {path}")
    return path


def _dist_files(out_dir: Path) -> tuple[Path, ...]:
    found = set(out_dir.glob("*.whl")) | set(out_dir.glob("*.tar.gz"))
    return tuple(sorted((path.resolve() for path in found), key=lambda path: path.name))


@dataclass(frozen=True)
class DistResult:
    root: str
    out_dir: str
    backend: str
    command: str
    files: tuple[Path, ...] = ()

    @property
    def count(self) -> int:
        return len(self.files)

    def to_dict(self) -> dict[str, object]:
        return {
            "root": self.root,
            "outDir": self.out_dir,
            "backend": self.backend,
            "command": self.command,
            "count": self.count,
            "files": [str(path) for path in self.files],
        }

    def summary(self) -> str:
        lines = [
            f"[✓] dist: {self.count} artefato(s) em {self.out_dir} "
            f"(backend: {self.backend})"
        ]
        lines.extend(f"    - {path.name}" for path in self.files)
        return "\n".join(lines)


@dataclass(frozen=True)
class CheckResult:
    checked: tuple[Path, ...]
    command: str

    @property
    def count(self) -> int:
        return len(self.checked)

    def to_dict(self) -> dict[str, object]:
        return {
            "count": self.count,
            "command": self.command,
            "files": [str(path) for path in self.checked],
        }

    def summary(self) -> str:
        return f"[✓] twine check: {self.count} artefato(s) válido(s)"


@dataclass(frozen=True)
class UploadResult:
    mode: str
    repository: str
    files: tuple[Path, ...]
    command: str
    credential_source: str

    @property
    def count(self) -> int:
        return len(self.files)

    def to_dict(self) -> dict[str, object]:
        return {
            "mode": self.mode,
            "repository": self.repository,
            "count": self.count,
            "command": self.command,
            "credentialSource": self.credential_source,
            "files": [str(path) for path in self.files],
        }

    def summary(self) -> str:
        return (
            f"[✓] publish-pypi: modo={self.mode}, repositório={self.repository}, "
            f"{self.count} artefato(s), credenciais={self.credential_source}\n"
            f"    comando: {self.command}"
        )


def build_dist(
    root: str | Path,
    out_dir: str | Path,
    *,
    use_build: bool | None = None,
    isolation: bool = True,
) -> DistResult:
    """Gera sdist + wheel.

    Usa ``python -m build`` quando o pacote está instalado. Se não estiver,
    usa o fallback legado ``setup.py sdist bdist_wheel`` (presente apenas em
    repositórios que ainda mantêm ``setup.py``).
    """
    root_path = Path(root).expanduser().resolve()
    out_path = Path(out_dir).expanduser().resolve()
    if not root_path.is_dir():
        raise PublishError(f"Raiz do projeto não é diretório: {root_path}")
    out_path.mkdir(parents=True, exist_ok=True)

    has_module = _has_build_module() if use_build is None else use_build
    if has_module:
        cmd = [
            sys.executable, "-m", "build",
            "--outdir", str(out_path),
            "--sdist", "--wheel",
        ]
        if not isolation:
            cmd.append("--no-isolation")
        cmd.append(str(root_path))
        backend = "python -m build"
    else:
        setup_py = root_path / "setup.py"
        if not setup_py.is_file():
            raise PublishError(
                "python -m build não está disponível e não há setup.py para o fallback. "
                "Instale com: python3 -m pip install build"
            )
        cmd = [
            sys.executable, "setup.py",
            "sdist", "--dist-dir", str(out_path),
            "bdist_wheel", "--dist-dir", str(out_path),
        ]
        backend = "setup.py sdist bdist_wheel"

    _run(cmd, cwd=root_path, what=backend)
    files = _dist_files(out_path)
    if not files:
        raise PublishError(f"{backend} terminou sem gerar .whl/.tar.gz em {out_path}")
    return DistResult(
        root=str(root_path),
        out_dir=str(out_path),
        backend=backend,
        command=_display(cmd),
        files=files,
    )


def check_dist(
    files: Iterable[str | Path],
    *,
    cwd: str | Path | None = None,
) -> CheckResult:
    """Valida metadados dos artefatos com ``twine check``."""
    paths = tuple(_as_file(path) for path in files)
    if not paths:
        raise PublishError("Nenhum artefato informado para twine check")
    cmd = [*_tool_prefix("twine"), "check", *(str(path) for path in paths)]
    _run(
        cmd,
        cwd=Path(cwd).expanduser().resolve() if cwd is not None else None,
        what="twine check",
    )
    return CheckResult(checked=paths, command=_display(cmd))


def _repository_args(repository: str | None) -> tuple[str, list[str]]:
    name = (repository or "").strip()
    if not name or name.lower() == "pypi":
        return "PyPI (padrão)", []
    if name.startswith("-"):
        raise PublishError("Nome de repositório inválido")
    if name.lower() in ("testpypi", "test-pypi"):
        return "TestPyPI", ["--repository-url", _TESTPYPI_URL]
    return name, ["--repository", name]


def _credentials(env: Mapping[str, str]) -> tuple[str, str, str] | None:
    username = (env.get("TWINE_USERNAME") or "").strip()
    password = env.get("TWINE_PASSWORD") or ""
    if username and password:
        return username, password, "env:TWINE_USERNAME/TWINE_PASSWORD"
    token = (env.get("TWINE_API_TOKEN") or env.get("PYPI_TOKEN") or "").strip()
    if token:
        source = "TWINE_API_TOKEN" if env.get("TWINE_API_TOKEN") else "PYPI_TOKEN"
        return "__token__", token, f"env:{source}"
    return None


def upload(
    files: Iterable[str | Path],
    *,
    repository: str | None = None,
    dry_run: bool = True,
    yes: bool = False,
    env: Mapping[str, str] | None = None,
    cwd: str | Path | None = None,
) -> UploadResult:
    """Envia artefatos com ``twine upload``.

    ``dry_run=True`` (padrão) apenas descreve o plano: nenhum subprocess de
    upload é executado. Um upload real exige ``dry_run=False`` **e** ``yes=True``.
    """
    paths = tuple(_as_file(path) for path in files)
    if not paths:
        raise PublishError("Nenhum artefato informado para upload")
    if not dry_run and not yes:
        raise PublishError("Upload real exige confirmação explícita (--yes)")

    label, repo_args = _repository_args(repository)
    cmd = [*_tool_prefix("twine"), "upload", *repo_args, *(str(path) for path in paths)]
    source_env = os.environ if env is None else env
    creds = _credentials(source_env)
    credential_source = creds[2] if creds else "não configuradas (dry-run)"

    if dry_run:
        return UploadResult("dry-run", label, paths, _display(cmd), credential_source)

    if creds is None:
        raise PublishError(
            "Token ausente. Defina TWINE_USERNAME/TWINE_PASSWORD ou "
            "TWINE_API_TOKEN/PYPI_TOKEN antes de usar --yes."
        )
    username, password, credential_source = creds
    child_env = dict(source_env)
    child_env["TWINE_USERNAME"] = username
    child_env["TWINE_PASSWORD"] = password
    # O twine lê só TWINE_USERNAME/TWINE_PASSWORD; não duplicar o segredo.
    child_env.pop("TWINE_API_TOKEN", None)
    child_env.pop("PYPI_TOKEN", None)

    _run(
        cmd,
        cwd=Path(cwd).expanduser().resolve() if cwd is not None else None,
        env=child_env,
        secrets=(password,),
        what="twine upload",
    )
    return UploadResult("upload", label, paths, _display(cmd), credential_source)


def _add_quiet(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "-q", "--quiet", dest="cmd_quiet", action="store_true",
        help="silencioso: mostra só erros e avisos de segurança",
    )


def _cmd_dist(args: argparse.Namespace) -> int:
    from .cli import _fail, _quiet

    try:
        built = build_dist(
            args.root,
            args.out_dir,
            use_build=False if args.no_build_module else None,
            isolation=not args.no_isolation,
        )
        checked = None if args.no_check else check_dist(built.files)
    except (PublishError, OSError, ValueError) as exc:
        return _fail(f"Falha ao gerar distribuição: {exc}")
    if not _quiet(args):
        print(built.summary())
        if checked is not None:
            print(checked.summary())
    return 0


def _cmd_publish_pypi(args: argparse.Namespace) -> int:
    from .cli import _fail, _quiet

    mode = "DRY-RUN (nenhum upload)" if args.dry_run else "UPLOAD REAL (autorizado por --yes)"
    print(f"[segurança] publish-pypi: {mode}; repositório={args.repository or 'pypi'}")
    try:
        out_path = Path(args.out_dir).expanduser().resolve()
        if args.skip_build:
            files = _dist_files(out_path)
            if not files:
                raise PublishError(f"Nenhum artefato em {out_path} (use sem --skip-build)")
            built = None
        else:
            built = build_dist(
                args.root,
                args.out_dir,
                use_build=False if args.no_build_module else None,
                isolation=not args.no_isolation,
            )
            files = built.files
        checked = None if args.no_check else check_dist(files)
        result = upload(
            files,
            repository=args.repository,
            dry_run=args.dry_run,
            yes=not args.dry_run,
        )
    except (PublishError, OSError, ValueError) as exc:
        return _fail(f"Falha ao publicar: {exc}")
    if not _quiet(args):
        if built is not None:
            print(built.summary())
        if checked is not None:
            print(checked.summary())
        print(result.summary())
    return 0


def register(subparsers: argparse._SubParsersAction) -> None:
    """Registra ``dist`` e ``publish-pypi`` no parser da CLI."""
    p_dist = subparsers.add_parser(
        "dist", help="gera sdist + wheel e valida com twine check"
    )
    p_dist.add_argument("root", nargs="?", default=".", help="raiz do repositório")
    p_dist.add_argument("-o", "--out-dir", default="dist", help="diretório de saída (padrão: dist)")
    p_dist.add_argument("--no-check", action="store_true", help="não executa twine check")
    p_dist.add_argument("--no-isolation", action="store_true", help="python -m build --no-isolation")
    p_dist.add_argument(
        "--no-build-module", action="store_true",
        help="força o fallback setup.py sdist bdist_wheel",
    )
    _add_quiet(p_dist)
    p_dist.set_defaults(func=_cmd_dist)

    p_pub = subparsers.add_parser(
        "publish-pypi",
        help="publica no PyPI/TestPyPI (DRY-RUN por padrão; --yes autoriza o upload)",
    )
    p_pub.add_argument("root", nargs="?", default=".", help="raiz do repositório")
    p_pub.add_argument("-o", "--out-dir", default="dist", help="diretório de artefatos")
    p_pub.add_argument(
        "--repository", default=None,
        help="pypi (padrão), testpypi ou nome configurado no ~/.pypirc",
    )
    mode = p_pub.add_mutually_exclusive_group()
    mode.add_argument(
        "--dry-run", dest="dry_run", action="store_true", default=True,
        help="padrão: valida e mostra o plano, sem enviar nada",
    )
    mode.add_argument(
        "--yes", dest="dry_run", action="store_false",
        help="autoriza upload real (exige token no ambiente)",
    )
    p_pub.add_argument(
        "--skip-build", action="store_true",
        help="reusa os artefatos existentes em --out-dir",
    )
    p_pub.add_argument("--no-check", action="store_true", help="não executa twine check")
    p_pub.add_argument("--no-isolation", action="store_true", help="build sem isolamento")
    p_pub.add_argument(
        "--no-build-module", action="store_true",
        help="força o fallback setup.py sdist bdist_wheel",
    )
    _add_quiet(p_pub)
    p_pub.set_defaults(func=_cmd_publish_pypi)
