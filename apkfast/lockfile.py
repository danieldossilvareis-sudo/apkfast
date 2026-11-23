"""Lockfile reprodutível da toolchain (``apkfast.lock.json``, lockVersion 1).

O arquivo fixa NDK, build-tools, API da platform/Android e versões/checksums
das dependências preparadas por ``setup-libs``. Ele é portátil: caminhos
absolutos **não** são gravados, apenas versões e revisões.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import tempfile
from pathlib import Path
from typing import Any

from . import __version__

LOCK_NAME = "apkfast.lock.json"
LOCK_VERSION = 1

COMPARED_FIELDS = (
    ("ndkVersion", "NDK"),
    ("buildTools", "build-tools"),
    ("platformApi", "API da platform"),
    ("ndkApi", "API do NDK/clang"),
    ("libwebp", "libwebp"),
    ("lunaSVG", "LunaSVG"),
)


class LockfileError(RuntimeError):
    """Lockfile ausente, corrompido ou divergente da toolchain."""


def path(project: Path | str) -> Path:
    """Caminho padrão do lockfile dentro de um projeto."""
    return Path(project) / LOCK_NAME


def _resolve(target: Path | str) -> Path:
    candidate = Path(target)
    if candidate.name == LOCK_NAME or candidate.suffix == ".json":
        return candidate
    return path(candidate)


def _sha256(path: Path) -> str | None:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


def _ndk_version(ndk_dir: Path) -> str:
    props = ndk_dir / "source.properties"
    if props.is_file():
        try:
            for line in props.read_text(encoding="utf-8", errors="replace").splitlines():
                if line.strip().startswith("Pkg.Revision"):
                    value = line.split("=", 1)[1].strip()
                    if value:
                        return value
        except OSError:
            pass
    match = re.search(r"(?:ndk-)?r?(\d+[a-z]?(?:\.\d+)*)", ndk_dir.name, re.IGNORECASE)
    if match:
        return ("r" if not match.group(1).lower().startswith("r") else "") + match.group(1)
    return ndk_dir.name or "unknown"


def _platform_api(platform_dir: Path, fallback: int) -> int:
    match = re.search(r"android-(\d+)", platform_dir.name)
    return int(match.group(1)) if match else int(fallback)


def _available_checksums() -> dict[str, str]:
    """Checksums que podem ser conferidos localmente (quando disponíveis)."""
    result: dict[str, str] = {}
    from . import lunasvg as luna_mod
    from . import webp as webp_mod

    result["libwebp.tarball"] = webp_mod.SHA256
    result["lunaSVG.tarball"] = luna_mod.SHA256
    patch = luna_mod.FILTERS_PATCH
    if patch.is_file():
        digest = _sha256(patch)
        if digest:
            result["lunaSVG.filtersPatch"] = digest
    return result


def facts(tc: Any, *, api: int | None = None) -> dict:
    """Extrai fatos versionados de uma toolchain detectada.

    ``tc`` pode ser um ``Toolchain`` real ou um objeto com os mesmos atributos
    (útil em testes e em ambientes sem SDK instalado).
    """
    requested_api = tc.api if api is None else int(api)
    ndk_dir = Path(tc.ndk_dir)
    build_tools = Path(tc.build_tools_dir).name
    platform_dir = Path(tc.platform_dir)
    info: dict[str, Any] = {
        "ndkVersion": _ndk_version(ndk_dir),
        "buildTools": build_tools,
        "platformApi": _platform_api(platform_dir, requested_api),
        "ndkApi": int(requested_api),
        "libwebp": _dependency_version("webp"),
        "lunaSVG": _dependency_version("lunasvg"),
    }
    return info


def _dependency_version(module: str) -> str:
    from importlib import import_module

    return str(getattr(import_module(f".{module}", __package__), "VERSION"))


def generate(project: Path | str, tc: Any, *, api: int | None = None) -> dict:
    """Cria o documento do lockfile a partir da toolchain detectada."""
    document: dict[str, Any] = {
        "lockVersion": LOCK_VERSION,
        "generatedBy": f"apkfast {__version__}",
        **facts(tc, api=api),
        "checksums": _available_checksums(),
    }
    return validate(document, path=path(project))


def validate(document: Any, *, path: Path | str | None = None) -> dict:
    """Valida a estrutura do lockfile; devolve uma cópia."""
    prefix = f"{path}: " if path is not None else ""
    if not isinstance(document, dict):
        raise LockfileError(f"{prefix}deve conter um objeto JSON.")
    version = document.get("lockVersion")
    if not isinstance(version, int) or isinstance(version, bool):
        raise LockfileError(f"{prefix}lockVersion deve ser um inteiro; encontrado {version!r}.")
    if version > LOCK_VERSION:
        raise LockfileError(
            f"{prefix}lockVersion {version} é mais novo que o suportado ({LOCK_VERSION}). "
            "Atualize o apkfast."
        )
    if version < 1:
        raise LockfileError(f"{prefix}lockVersion inválido: {version}.")

    errors: list[str] = []
    for key, _ in COMPARED_FIELDS:
        value = document.get(key)
        if key in ("platformApi", "ndkApi"):
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                errors.append(f"'{key}' deve ser um inteiro positivo; encontrado {value!r}.")
        elif not isinstance(value, str) or not value.strip():
            errors.append(f"'{key}' deve ser um texto não vazio; encontrado {value!r}.")
    checksums = document.get("checksums", {})
    if not isinstance(checksums, dict) or any(
        not isinstance(k, str)
        or not isinstance(v, str)
        or not re.fullmatch(r"[0-9a-f]{64}", v)
        for k, v in (checksums.items() if isinstance(checksums, dict) else [])
    ):
        errors.append("'checksums' deve ser um objeto de textos SHA256 (64 caracteres hex).")
    if errors:
        raise LockfileError(prefix + " ".join(errors))
    return dict(document)


def load(target: Path | str, *, required: bool = False) -> dict | None:
    """Lê e valida o lockfile de um projeto (ou um arquivo explícito)."""
    lock_path = _resolve(target)
    if not lock_path.is_file():
        if required:
            raise LockfileError(f"Lockfile não encontrado: {lock_path}")
        return None
    try:
        data = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise LockfileError(f"{lock_path}: JSON inválido: {exc}") from exc
    return validate(data, path=lock_path)


def write(project: Path | str, document: dict, *, output: Path | str | None = None) -> Path:
    """Grava o lockfile de forma atômica e devolve o caminho usado."""
    validated = validate(document)
    destination = _resolve(output) if output is not None else path(project)
    tmp: Path | None = None
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=destination.parent,
            prefix=".apkfast-lock-",
            suffix=".json",
            delete=False,
        ) as handle:
            tmp = Path(handle.name)
            json.dump(validated, handle, indent=2, sort_keys=True)
            handle.write("\n")
        os.replace(tmp, destination)
    except OSError as exc:
        raise LockfileError(f"Falha ao gravar {destination}: {exc}") from exc
    finally:
        if tmp is not None:
            tmp.unlink(missing_ok=True)
    return destination


def compare(document: dict, actual: dict) -> list[str]:
    """Lista divergências entre o lockfile e a toolchain detectada."""
    validated = validate(document)
    detected = dict(actual)
    mismatches: list[str] = []
    for key, label in COMPARED_FIELDS:
        expected, found = validated.get(key), detected.get(key)
        if expected != found:
            mismatches.append(f"{label}: lockfile={expected!r}, detectado={found!r}")

    available = _available_checksums()
    for key, expected in sorted(validated.get("checksums", {}).items()):
        if key in available and available[key] != expected:
            mismatches.append(
                f"checksum {key}: lockfile={expected!r}, instalado={available[key]!r}"
            )
    return mismatches


def verify(document: dict, actual: dict, *, ignore: bool = False) -> list[str]:
    """Falha quando a toolchain diverge do lockfile (a menos que ``ignore``)."""
    mismatches = compare(document, actual)
    if mismatches and not ignore:
        detail = "\n".join(f"  - {item}" for item in mismatches)
        raise LockfileError(
            "apkfast.lock.json divergiu da toolchain detectada:\n"
            f"{detail}\n"
            "Atualize o lockfile com 'apkfast build . --generate-lock' ou use "
            "--ignore-lock para seguir com a toolchain detectada."
        )
    return mismatches
