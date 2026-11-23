"""Transporte de bundles de cache (file://, HTTP(S) e s3://).

Este módulo NÃO define formato de bundle: ele apenas move o ``.tar.gz``
produzido por :func:`apkfast.cache.export_bundle` para
:func:`apkfast.cache.import_bundle`. Nenhuma extração acontece aqui.

Segurança:

- ``file://`` e paths locais usam cópia para ``.part`` + ``os.replace``;
- HTTP(S) usa ``curl -fL --retry 3`` (PUT/GET) e restrige redirecionamentos
  a ``http/https`` para não virar ``file:``;
- ``s3://`` usa ``aws s3 cp`` (CLI already configured). ETags S3 **nunca**
  são usados como checksum: o SHA256 opcional (``--expect-sha256``) é do
  arquivo de bundle inteiro, e a validação real é a de ``import_bundle``;
- HMAC é o de ``apkfast.cache`` (``APKFAST_CACHE_HMAC``).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable
from urllib.parse import unquote, urlparse

from . import cache as cache_mod

__all__ = ["register", "push", "pull", "TransportError", "TransportResult", "Location"]

_SHA256 = re.compile(r"^[0-9a-fA-F]{64}$")


class TransportError(RuntimeError):
    pass


@dataclass(frozen=True)
class Location:
    """Destino/fonte normalizado. ``path`` é preenchido só para ``file``."""

    scheme: str
    url: str
    path: Path | None = None

    @property
    def display(self) -> str:
        return str(self.path) if self.scheme == "file" else self.url


@dataclass
class TransportResult:
    action: str
    source: str
    destination: str
    scheme: str
    bytes: int = 0
    sha256: str = ""
    stats: dict[str, object] = field(default_factory=dict)

    def summary(self) -> str:
        short = self.sha256[:16] if self.sha256 else "-"
        if self.action == "push":
            blobs = self.stats.get("blobs", 0)
            abis = self.stats.get("abis", [])
            abi_text = ", ".join(str(abi) for abi in abis) or "-"
            return (
                f"[✓] cache-push: {self.destination} "
                f"({blobs} blobs, ABIs: {abi_text}, {self.bytes} bytes, sha256={short}…)"
            )
        return (
            f"[✓] cache-pull: {self.source} -> {self.destination} "
            f"({self.stats.get('imported', 0)} novo(s), "
            f"{self.stats.get('skipped', 0)} já existente(s), "
            f"{self.stats.get('rejected', 0)} rejeitado(s), sha256={short}…)"
        )

    def __str__(self) -> str:
        return self.summary()


def _text(value: str | Path) -> str:
    if isinstance(value, Path):
        text = str(value)
    elif isinstance(value, str):
        text = value.strip()
    else:
        raise TransportError(f"Local inválido: {value!r}")
    if not text:
        raise TransportError("Local vazio")
    if any(char in text for char in ("\0", "\n", "\r")):
        raise TransportError("Local contém caractere de controle")
    return text


def parse_location(raw: str | Path) -> Location:
    """Aceita path local, file://, http://, https:// e s3://."""
    text = _text(raw)
    if text.startswith("file://"):
        parsed = urlparse(text)
        if parsed.netloc not in ("", "localhost"):
            raise TransportError(f"file:// com host remoto não é suportado: {parsed.netloc}")
        if parsed.query or parsed.fragment:
            raise TransportError("file:// não aceita query ou fragmento")
        path = Path(unquote(parsed.path)).expanduser().resolve()
        return Location("file", text, path)
    if text.startswith(("http://", "https://")):
        parsed = urlparse(text)
        if not parsed.netloc or parsed.username or parsed.password:
            raise TransportError("URL HTTP(S) inválida ou com credenciais embutidas")
        if parsed.fragment:
            raise TransportError("URL HTTP(S) não aceita fragmento")
        return Location(parsed.scheme, text)
    if text.startswith("s3://"):
        parsed = urlparse(text)
        if not parsed.netloc or not parsed.path.strip("/") or parsed.username or parsed.password:
            raise TransportError("Use s3://<bucket>/<chave>")
        if parsed.query or parsed.fragment:
            raise TransportError("s3:// não aceita query ou fragmento")
        return Location("s3", text)
    path = Path(text).expanduser().resolve()
    return Location("file", path.as_uri(), path)


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _tool(name: str, explicit: str | Path | None) -> str:
    found = str(explicit) if explicit else shutil.which(name)
    if not found:
        raise TransportError(f"{name} não encontrado no PATH (use o parâmetro {name}_path)")
    return found


def _run(cmd: list[str], what: str) -> None:
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    except OSError as exc:
        raise TransportError(f"Falha ao executar {what}: {exc}") from exc
    except subprocess.SubprocessError as exc:  # pragma: no cover - defensivo
        raise TransportError(f"Falha ao executar {what}: {exc}") from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip().splitlines()
        tail = " | ".join(detail[-3:]) if detail else "sem saída"
        raise TransportError(f"{what} falhou ({result.returncode}): {tail}")


def _curl_common(exe: str, timeout: int, headers: Iterable[str]) -> list[str]:
    command = [
        exe, "-fL", "--retry", "3", "--retry-delay", "1",
        "--connect-timeout", "15", "--max-time", str(timeout), "-sS",
        "--proto", "=http,https", "--proto-redir", "=http,https",
    ]
    for header in headers:
        value = _text(header)
        if value.startswith("-") or "\"" in value:
            raise TransportError("Header HTTP inválido")
        command += ["-H", value]
    return command


def _stage_download(head: list[str], loc: Location, target: Path, *,
                    aws: str | None, timeout: int) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    part = target.with_name(target.name + f".part-{uuid.uuid4().hex}")
    try:
        if loc.scheme in ("http", "https"):
            cmd = head + ["-o", str(part), "--", loc.url]
        elif loc.scheme == "s3":
            aws_exe = _tool("aws", aws)
            cmd = [aws_exe, "s3", "cp", loc.url, str(part), "--only-show-errors"]
        else:
            if not loc.path or not loc.path.is_file():
                raise TransportError(f"Arquivo de cache não encontrado: {loc.path}")
            shutil.copyfile(loc.path, part)
            cmd = []
        if cmd:
            _run(cmd, f"download {loc.scheme}")
        if not part.is_file() or part.stat().st_size == 0:
            raise TransportError(f"Bundle vazio ou não recebido: {loc.display}")
        os.replace(part, target)
    except OSError as exc:
        raise TransportError(f"Falha ao gravar bundle local: {exc}") from exc
    finally:
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass


def _send_file(bundle: Path, loc: Location, *, curl: str | None,
               aws: str | None, headers: Iterable[str], timeout: int) -> None:
    if loc.scheme == "file":
        assert loc.path is not None
        destination = loc.path
        if destination.is_dir():
            raise TransportError(f"Destino é diretório: {destination}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        part = destination.with_name(destination.name + f".tmp-{uuid.uuid4().hex}")
        try:
            shutil.copyfile(bundle, part)
            os.replace(part, destination)
        except OSError as exc:
            raise TransportError(f"Falha ao copiar bundle para {destination}: {exc}") from exc
        finally:
            try:
                part.unlink(missing_ok=True)
            except OSError:
                pass
        return
    if loc.scheme in ("http", "https"):
        exe = _tool("curl", curl)
        cmd = _curl_common(exe, timeout, headers) + [
            "-X", "PUT", "--upload-file", str(bundle), "--", loc.url,
        ]
        _run(cmd, "upload http")
        return
    aws_exe = _tool("aws", aws)
    _run([aws_exe, "s3", "cp", str(bundle), loc.url, "--only-show-errors"], "upload s3")


def _check_signature_env(*, required: bool) -> None:
    if required and not os.environ.get("APKFAST_CACHE_HMAC"):
        raise TransportError(
            "--require-cache-signature exige APKFAST_CACHE_HMAC no ambiente"
        )


def push(ctx: cache_mod.CacheContext, destination: str | Path, *,
         abis: Iterable[str] | None = None,
         headers: Iterable[str] = (),
         curl: str | Path | None = None,
         aws: str | Path | None = None,
         timeout: int = 120) -> TransportResult:
    """Exporta o cache e envia o bundle para ``destination``.

    O bundle é gerado por ``cache.export_bundle`` num diretório temporário;
    nada é recompactado nem reformatado.
    """
    loc = parse_location(destination)
    names = sorted(abis or ctx.abis)
    timeout = max(1, int(timeout))
    _check_signature_env(required=ctx.require_signature)

    with tempfile.TemporaryDirectory(prefix="apkfast-cache-push-") as scratch:
        bundle = Path(scratch) / "bundle.tar.gz"
        locks = cache_mod.acquire_all(ctx, names)
        try:
            manifest = cache_mod.export_bundle(ctx, bundle, names)
        finally:
            cache_mod.release_all(locks)
        _send_file(bundle, loc, curl=curl, aws=aws, headers=headers, timeout=timeout)
        return TransportResult(
            action="push",
            source=f"cache:{ctx.root}",
            destination=loc.display,
            scheme=loc.scheme,
            bytes=bundle.stat().st_size,
            sha256=_sha256_file(bundle),
            stats={"blobs": len(manifest.get("blobs", {})), "abis": names},
        )


def pull(ctx: cache_mod.CacheContext, source: str | Path, *,
         abis: Iterable[str] | None = None,
         force: bool = False,
         require_signature: bool | None = None,
         expect_sha256: str | None = None,
         headers: Iterable[str] = (),
         curl: str | Path | None = None,
         aws: str | Path | None = None,
         timeout: int = 120) -> TransportResult:
    """Recebe o bundle e importa via ``cache.import_bundle``.

    ``expect_sha256`` é opcional e compara o SHA256 do arquivo ``.tar.gz``;
    ETags de S3 nunca são consultados.
    """
    loc = parse_location(source)
    names = sorted(abis or ctx.abis)
    timeout = max(1, int(timeout))
    required = ctx.require_signature if require_signature is None else bool(require_signature)
    _check_signature_env(required=required)
    if expect_sha256 is not None:
        expected = expect_sha256.strip().lower()
        if not _SHA256.fullmatch(expected):
            raise TransportError("--expect-sha256 deve ter 64 dígitos hexadecimais")

    with tempfile.TemporaryDirectory(prefix="apkfast-cache-pull-") as scratch:
        bundle = Path(scratch) / "bundle.tar.gz"
        head = _curl_common(_tool("curl", curl), timeout, headers) if loc.scheme in ("http", "https") else []
        _stage_download(head, loc, bundle, aws=aws, timeout=timeout)
        digest = _sha256_file(bundle)
        if expect_sha256 is not None and digest != expected:
            raise TransportError(
                f"SHA256 do bundle diverge: esperado {expected}, obtido {digest}"
            )
        locks = cache_mod.acquire_all(ctx, names)
        try:
            stats = cache_mod.import_bundle(
                ctx, bundle, abis=names, force=force, require_signature=required
            )
        finally:
            cache_mod.release_all(locks)
        return TransportResult(
            action="pull",
            source=loc.display,
            destination=f"cache:{ctx.root}",
            scheme=loc.scheme,
            bytes=bundle.stat().st_size,
            sha256=digest,
            stats=dict(stats),
        )


def _add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--cache-dir", help="raiz do cache (sobrescreve apkfast.json)")
    parser.add_argument("--cache-mode", choices=("trusted", "verify"), default=None)
    parser.add_argument("--cache-namespace", default="")
    parser.add_argument(
        "--require-cache-signature", action="store_true", default=None,
        help="exige assinatura HMAC no bundle recebido",
    )
    parser.add_argument("--abi", default="arm64-v8a", help="ABI alvo ou all")
    parser.add_argument("--abis", default="", help="lista de ABIs separadas por vírgula")
    parser.add_argument(
        "--header", action="append", default=[], dest="header",
        help="header HTTP extra para curl (repitível; não é exibido)",
    )
    parser.add_argument(
        "--timeout", type=int, default=120, help="timeout por operação em segundos"
    )
    parser.add_argument(
        "-q", "--quiet", dest="cmd_quiet", action="store_true",
        help="silencioso: mostra só erros",
    )


def _cmd_cache_push(args: argparse.Namespace) -> int:
    from .cli import _cache_transport_context, _fail, _quiet

    try:
        ctx, abis = _cache_transport_context(args)
        result = push(
            ctx, args.destination, abis=abis, headers=args.header or (),
            timeout=args.timeout,
        )
    except (ValueError, TransportError, cache_mod.CacheError, OSError) as exc:
        return _fail(f"Falha ao enviar cache: {exc}")
    if not _quiet(args):
        print(result.summary())
    return 0


def _cmd_cache_pull(args: argparse.Namespace) -> int:
    from .cli import _cache_transport_context, _fail, _quiet

    try:
        ctx, abis = _cache_transport_context(args)
        result = pull(
            ctx, args.source, abis=abis, force=args.force,
            require_signature=args.require_cache_signature,
            expect_sha256=args.expect_sha256, headers=args.header or (),
            timeout=args.timeout,
        )
    except (ValueError, TransportError, cache_mod.CacheError, OSError) as exc:
        return _fail(f"Falha ao receber cache: {exc}")
    if not _quiet(args):
        print(result.summary())
    return 0


def register(subparsers: argparse._SubParsersAction) -> None:
    """Registra ``cache-push`` e ``cache-pull`` no parser da CLI."""
    push_parser = subparsers.add_parser(
        "cache-push", help="exporta e envia o bundle de cache para file/http(s)/s3"
    )
    push_parser.add_argument("destination", help="file://..., http(s)://..., s3://... ou path local")
    push_parser.add_argument("project", nargs="?", default=".", help="diretório do projeto")
    _add_common_args(push_parser)
    push_parser.set_defaults(func=_cmd_cache_push)

    pull_parser = subparsers.add_parser(
        "cache-pull", help="recebe e importa o bundle de cache de file/http(s)/s3"
    )
    pull_parser.add_argument("source", help="file://..., http(s)://..., s3://... ou path local")
    pull_parser.add_argument("project", nargs="?", default=".", help="diretório do projeto")
    pull_parser.add_argument("--force", action="store_true", help="aceita projectId diferente")
    pull_parser.add_argument(
        "--expect-sha256", help="SHA256 esperado do .tar.gz (opcional; não é ETag)"
    )
    _add_common_args(pull_parser)
    pull_parser.set_defaults(func=_cmd_cache_pull)
