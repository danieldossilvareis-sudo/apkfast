"""Helpers para assinatura de APK (apksigner) e versionCode.

Este módulo é deliberadamente livre de efeitos colaterais: resolução de
opiniões de keystore, montagem da linha de comando do ``apksigner``, cálculo de
``versionCode`` e digest da keystore são funções puras (com injeção opcional de
``help_text``/``now_ms`` para testes). Nenhuma senha é gravada, retornada em
mensagens de erro ou usada em avisos.

A integração com ``builder.py``/``cli.py`` é feita por quem orquestra o build;
aqui só estão as primitivas testáveis.
"""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import time
import warnings
import xml.etree.ElementTree as ET
from collections.abc import Callable, Mapping
from dataclasses import dataclass, field
from pathlib import Path

__all__ = [
    "DEFAULT_KEY_ALIAS",
    "DEFAULT_KEY_PASS",
    "DEFAULT_STORE_PASS",
    "ENV_KEY_ALIAS",
    "ENV_KEYSTORE",
    "ENV_KEY_PASS",
    "ENV_STORE_PASS",
    "MAX_VERSION_CODE",
    "KeystoreOptions",
    "SigningError",
    "SigningWarning",
    "TIMESTAMP_BASE_MS",
    "apksigner_help",
    "apksigner_sign_args",
    "detect_supported_versions",
    "next_version_code",
    "redact",
    "redact_args",
    "resolve_keystore_options",
    "signature_fingerprint",
]

ENV_KEYSTORE = "APKFAST_KEYSTORE"
ENV_STORE_PASS = "APKFAST_STORE_PASS"
ENV_KEY_PASS = "APKFAST_KEY_PASS"
ENV_KEY_ALIAS = "APKFAST_KEY_ALIAS"

# Valores usados apenas quando nem CLI nem ambiente fornecem a opção.
DEFAULT_STORE_PASS = "android"
DEFAULT_KEY_PASS = "android"
DEFAULT_KEY_ALIAS = "androiddebugkey"

# Google Play aceita no máximo 2100000000 (int32 com folga); o Android aceita
# 2147483647, mas usar o limite da loja evita rejeição na publicação.
MAX_VERSION_CODE = 2_100_000_000

# Base da estratégia ``timestamp``: 2020-01-01T00:00:00Z em ms desde a epoch
# Unix. Subtraindo esta base, o versionCode começa em 0 (clampado a 1) em 2020
# e só atinge 2_100_000_000 após 2_100_000_000 s (~66,5 anos), por volta de
# 2086. Usar a epoch diretamente (1970) alcançaria o limite em 2036.
TIMESTAMP_BASE_MS = 1_577_836_800_000

_ANDROID_NS = "http://schemas.android.com/apk/res/android"
_SHA_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_INT_RE = re.compile(r"^[+-]?\d+$")

_SIGNING_FLAGS = {
    "v1": "--v1-signing-enabled",
    "v2": "--v2-signing-enabled",
    "v3": "--v3-signing-enabled",
    "v4": "--v4-signing-enabled",
}
_SECRET_FLAGS = frozenset({"--ks-pass", "--key-pass"})


class SigningError(RuntimeError):
    """Configuração de assinatura, versionCode ou keystore inválida."""


class SigningWarning(UserWarning):
    """Recurso de assinatura indisponível na instalação atual do apksigner."""


@dataclass(frozen=True)
class KeystoreOptions:
    """Opções resolvidas; use :meth:`redacted` para registrar em logs."""

    keystore: Path | None
    store_pass: str = field(repr=False)
    key_pass: str = field(repr=False)
    key_alias: str

    def redacted(self) -> dict[str, str]:
        return {
            "keystore": str(self.keystore) if self.keystore is not None else "<debug>",
            "store_pass": redact(self.store_pass),
            "key_pass": redact(self.key_pass),
            "key_alias": self.key_alias or "<unset>",
        }


def redact(value: object | None) -> str:
    """Máscara segura para senhas (nunca devolve o valor original)."""
    if value is None or not str(value):
        return "<unset>"
    return "***"


def redact_args(args: list[str] | tuple[str, ...]) -> list[str]:
    """Cópia de uma linha de comando com valores de --ks-pass/--key-pass mascarados."""
    result: list[str] = []
    hide_next = False
    for raw in args:
        item = str(raw)
        if hide_next:
            result.append(redact(item))
            hide_next = False
            continue
        head, sep, _tail = item.partition("=")
        if head in _SECRET_FLAGS:
            result.append(f"{head}={redact(_tail)}" if sep else item)
            if not sep:
                hide_next = True
            continue
        result.append(item)
        if item in _SECRET_FLAGS:
            hide_next = True
    return result


def _text(value: object | None) -> str | None:
    if value is None:
        return None
    text = str(value).strip()
    return text or None


def resolve_keystore_options(
    *,
    keystore: str | Path | None = None,
    store_pass: str | None = None,
    key_pass: str | None = None,
    key_alias: str | None = None,
    env: Mapping[str, str] | None = None,
    release: bool = False,
) -> KeystoreOptions:
    """Resolve CLI > ``APKFAST_*`` > padrões de debug.

    Um valor ``None`` (ou texto vazio) significa "não informado". Por isso as
    opções de CLI devem usar ``default=None`` no argparse: um default literal
    como ``"android"`` mascararia as variáveis de ambiente. Erros nunca incluem
    valores de senha, apenas os nomes das opções/variáveis.
    """

    environ: Mapping[str, str] = os.environ if env is None else env

    def pick(cli_value: object | None, env_name: str, default: str | None = None) -> str | None:
        return _text(cli_value) or _text(environ.get(env_name)) or default

    ks_raw = pick(keystore, ENV_KEYSTORE)
    resolved = KeystoreOptions(
        keystore=Path(ks_raw).expanduser() if ks_raw else None,
        store_pass=pick(store_pass, ENV_STORE_PASS, DEFAULT_STORE_PASS) or DEFAULT_STORE_PASS,
        key_pass=pick(key_pass, ENV_KEY_PASS, DEFAULT_KEY_PASS) or DEFAULT_KEY_PASS,
        key_alias=pick(key_alias, ENV_KEY_ALIAS, DEFAULT_KEY_ALIAS) or DEFAULT_KEY_ALIAS,
    )
    if release and resolved.keystore is None:
        raise SigningError(
            f"Modo release exige --keystore ou {ENV_KEYSTORE} "
            "(use uma upload key; nunca a chave de app signing da Play Store)."
        )
    return resolved


def _strip_ns(tag: str) -> str:
    return tag.split("}", 1)[-1] if "}" in tag else tag


def detect_supported_versions(help_text: str) -> dict[str, bool]:
    """Lê a saída de ``apksigner --help`` e detecta cada schema de assinatura."""

    text = str(help_text or "")
    return {
        name: re.search(rf"(?<![\w-]){re.escape(flag)}(?![\w-])", text) is not None
        for name, flag in _SIGNING_FLAGS.items()
    }


def apksigner_help(apksigner: str | Path, *, timeout: float = 15.0) -> str:
    """Obtém a saída de ``apksigner --help`` (sempre sem segredos na linha)."""

    try:
        result = subprocess.run(
            [str(apksigner), "--help"],
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise SigningError(
            f"Não foi possível consultar --help de apksigner ({type(exc).__name__})."
        ) from exc
    return f"{result.stdout or ''}\n{result.stderr or ''}"


def apksigner_sign_args(
    *,
    keystore: str | Path,
    store_pass: str,
    key_pass: str,
    key_alias: str,
    unsigned: str | Path,
    output: str | Path,
    apksigner: str | Path | None = None,
    v1: bool = True,
    v2: bool = True,
    v3: bool = True,
    v4: bool = True,
    help_text: str | None = None,
    probe: bool = True,
    warn: Callable[[str], None] | None = None,
) -> list[str]:
    """Monta ``apksigner sign`` com v1/v2/v3/v4 ligados por padrão.

    ``help_text`` pode ser injetado para evitar subprocess (tests/CI hermético).
    Quando ``apksigner`` é informado e ``help_text`` não é, a saída de ``--help``
    é consultada uma vez; recursos não encontrados são omitidos da linha com um
    aviso. A linha devolvida contém senhas: use :func:`redact_args` para logs.
    """

    if not _text(keystore):
        raise SigningError("Keystore é obrigatória para apksigner sign.")
    if not _text(key_alias):
        raise SigningError("Alias da chave é obrigatório para apksigner sign.")

    def emit(message: str) -> None:
        if warn is not None:
            warn(message)
        else:
            warnings.warn(message, SigningWarning, stacklevel=2)

    wanted = {"v1": bool(v1), "v2": bool(v2), "v3": bool(v3), "v4": bool(v4)}
    supported: dict[str, bool] | None = None
    if help_text is not None:
        supported = detect_supported_versions(help_text)
    elif apksigner is not None and probe:
        try:
            supported = detect_supported_versions(apksigner_help(apksigner))
        except SigningError as exc:
            # Sem --help confiável, mantemos v1/v2 (suporte universal) e
            # desativamos v3/v4 com aviso em vez de falhar o build.
            supported = {"v1": True, "v2": True, "v3": False, "v4": False}
            emit(f"[!] {exc} Assinaturas v3/v4 desativadas neste build.")

    if supported is not None:
        for name, enabled in wanted.items():
            if enabled and not supported.get(name, False):
                emit(
                    f"[!] apksigner sem suporte a {_SIGNING_FLAGS[name]}; "
                    f"assinatura v{name[1:]} desativada."
                )
                wanted[name] = False

    cmd = [
        str(apksigner) if apksigner is not None else "apksigner",
        "sign",
        "--ks",
        str(keystore),
        "--ks-pass",
        f"pass:{store_pass}",
        "--ks-key-alias",
        str(key_alias),
        "--key-pass",
        f"pass:{key_pass}",
    ]
    for name in ("v1", "v2", "v3", "v4"):
        if wanted[name] or (supported is not None and supported.get(name, False)):
            cmd += [_SIGNING_FLAGS[name], "true" if wanted[name] else "false"]
    cmd += ["--out", str(output), str(unsigned)]
    return cmd


def _manifest_version_code(manifest_path: str | Path) -> str:
    path = Path(manifest_path)
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        raise SigningError(f"AndroidManifest.xml inválido/ilegível ({path}): {exc}") from exc
    if _strip_ns(root.tag) != "manifest":
        raise SigningError(f"Raiz do manifesto deve ser <manifest>: {path}")
    value = root.attrib.get(f"{{{_ANDROID_NS}}}versionCode")
    if value is None:
        for key, candidate in root.attrib.items():
            if _strip_ns(key) == "versionCode":
                value = candidate
                break
    if value is None or not str(value).strip():
        raise SigningError(
            f"AndroidManifest.xml sem android:versionCode ({path}); "
            "use strategy='explicit'."
        )
    return str(value).strip()


def _checked_version_code(value: object, source: str) -> int:
    text = str(value).strip()
    if not _INT_RE.fullmatch(text):
        raise SigningError(f"versionCode de {source} não é inteiro: {text!r}")
    number = int(text)
    if not (1 <= number <= MAX_VERSION_CODE):
        raise SigningError(
            f"versionCode de {source} fora do intervalo 1..{MAX_VERSION_CODE}: {number}"
        )
    return number


def _timestamp_version_code(now_ms: int | None) -> int:
    milliseconds = int(time.time() * 1000) if now_ms is None else int(now_ms)
    if milliseconds < TIMESTAMP_BASE_MS:
        raise SigningError(
            "Timestamp para versionCode anterior a 2020-01-01 UTC "
            f"(base {TIMESTAMP_BASE_MS} ms); use strategy='explicit'."
        )
    # Segundos desde 2020-01-01 UTC, truncados para baixo: monotônico por
    # segundo e com ~66 anos de folga até o limite da Play (ver módulo).
    seconds = (milliseconds - TIMESTAMP_BASE_MS) // 1000
    if seconds > MAX_VERSION_CODE:
        raise SigningError(
            "Timestamp fora do limite de versionCode do Android "
            f"(máx. {MAX_VERSION_CODE}); use strategy='explicit'."
        )
    return max(seconds, 1)


def next_version_code(
    manifest_path: str | Path,
    strategy: str = "manifest+1",
    explicit: int | str | None = None,
    timestamp: bool = False,
    *,
    now_ms: int | None = None,
) -> int:
    """Devolve um ``versionCode`` inteiro para o manifesto informado.

    Estratégias: ``manifest+1`` (padrão), ``explicit`` (usa ``explicit``) e
    ``timestamp`` (segundos desde 2020-01-01 UTC, com resolução de 1 s,
    válidos até por volta de 2086 dentro do limite da Play). ``timestamp=True``
    é alias legado para a estratégia ``timestamp`` quando nenhuma estratégia
    explícita foi pedida.
    """

    name = str(strategy or "").strip().lower()
    if timestamp and name in ("", "manifest+1"):
        name = "timestamp"

    if name == "manifest+1":
        raw = _manifest_version_code(manifest_path)
        value = _checked_version_code(raw, "AndroidManifest.xml") + 1
    elif name == "explicit":
        if explicit is None:
            raise SigningError("strategy='explicit' exige explicit=<inteiro>.")
        value = _checked_version_code(explicit, "explicit")
    elif name == "timestamp":
        value = _timestamp_version_code(now_ms)
    else:
        raise SigningError(
            f"Estratégia de versionCode desconhecida: {strategy!r}. "
            "Use 'manifest+1', 'explicit' ou 'timestamp'."
        )

    if not (1 <= value <= MAX_VERSION_CODE):
        raise SigningError(
            f"versionCode resultante fora do intervalo 1..{MAX_VERSION_CODE}: {value}"
        )
    return value


def signature_fingerprint(keystore_path: str | Path) -> str:
    """SHA256 da keystore (hex), para compor a chave de cache do APK assinado.

    É um digest do arquivo da keystore — não o fingerprint do certificado — e
    muda quando a keystore é substituída ou rotacionada.
    """

    path = Path(keystore_path).expanduser()
    if not path.is_file():
        raise SigningError(f"Keystore não encontrada: {path}")
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise SigningError(f"Falha ao ler keystore ({path}): {exc}") from exc
    return digest.hexdigest()
