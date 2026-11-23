"""Validação do ``apkfast.json`` com mensagens claras e compatível com v2.3.

O schema é deliberadamente pequeno: ele cobre **todas** as chaves já lidas pelo
builder/CLI e nada mais. Chaves desconhecidas são erro (com sugestão de nome),
em vez de serem ignoradas silenciosamente.
"""

from __future__ import annotations

import difflib
import json
from pathlib import Path
from typing import Any

CONFIG_NAME = "apkfast.json"
TEMPLATES = ("nativeapp-gl", "nativeapp-vulkan", "raylib", "sdl2", "sdl3", "bare")
OPTIMIZATIONS = ("none", "release", "size")
CACHE_MODES = ("trusted", "verify")


class ConfigError(ValueError):
    """Configuração de projeto inválida."""


def _string(value: Any, *, allow_empty: bool = True) -> bool:
    return isinstance(value, str) and (allow_empty or bool(value.strip()))


def _integer(value: Any) -> bool:
    # bool é subclass de int em Python: não aceitar True como API 1.
    return isinstance(value, int) and not isinstance(value, bool)


def _string_list(value: Any) -> bool:
    return isinstance(value, list) and all(_string(item, allow_empty=False) for item in value)


# nome -> (validador, descrição curta)
FIELDS: dict[str, tuple[Any, str]] = {
    "nativeLibraryName": (lambda v: _string(v, allow_empty=False), "texto não vazio"),
    "abi": (
        lambda v: _string(v, allow_empty=False)
        or (isinstance(v, list) and bool(v) and _string_list(v)),
        "texto (ex.: 'arm64-v8a' ou 'arm64-v8a,armeabi-v7a') ou lista de textos",
    ),
    "api": (lambda v: _integer(v) and v > 0, "um inteiro positivo (API do NDK)"),
    "minSdk": (lambda v: _integer(v) and v > 0, "um inteiro positivo (minSdkVersion)"),
    "targetSdk": (lambda v: _integer(v) and v > 0, "um inteiro positivo (targetSdkVersion)"),
    "optimize": (lambda v: _string(v) and v in OPTIMIZATIONS, f"um de: {', '.join(OPTIMIZATIONS)}"),
    "obfuscate": (lambda v: isinstance(v, bool), "true ou false"),
    "release": (lambda v: isinstance(v, bool), "true ou false"),
    "keystore": (lambda v: _string(v, allow_empty=False), "texto não vazio (caminho do keystore)"),
    "template": (lambda v: _string(v) and v in TEMPLATES, f"um de: {', '.join(TEMPLATES)}"),
    "libraries": (_string_list, "uma lista de textos (caminhos de .a/.so)"),
    "libsDir": (lambda v: _string(v, allow_empty=False), "texto não vazio"),
    "incremental": (lambda v: isinstance(v, bool), "true ou false"),
    "cleanCache": (lambda v: isinstance(v, bool), "true ou false"),
    "cacheDir": (lambda v: _string(v, allow_empty=False), "texto não vazio (diretório do cache)"),
    "cacheMode": (lambda v: _string(v) and v in CACHE_MODES, f"um de: {', '.join(CACHE_MODES)}"),
    "cacheNamespace": (lambda v: _string(v), "um texto"),
    "requireCacheSignature": (lambda v: isinstance(v, bool), "true ou false"),
}

# Valores usados por `config show` quando a chave não está no apkfast.json.
# Espelham os defaults do builder/CLI; nenhum valor é aplicado ao build por
# este módulo (a precedência continua CLI > apkfast.json > padrão).
DEFAULTS: dict[str, Any] = {
    "nativeLibraryName": "main",
    "abi": "arm64-v8a",
    "api": 36,
    "minSdk": 21,
    "targetSdk": 36,
    "optimize": "release",
    "obfuscate": False,
    "release": False,
    "libraries": [],
    "libsDir": "libs",
    "incremental": True,
    "cleanCache": False,
    "cacheMode": "trusted",
    "cacheNamespace": "",
    "requireCacheSignature": False,
}


def _type_name(value: Any) -> str:
    return type(value).__name__


def validate(data: Any, *, path: Path | str | None = None) -> dict:
    """Valida um ``apkfast.json`` já decodificado e devolve uma cópia.

    Levanta :class:`ConfigError` apontando arquivo/chave/valor. O dicionário
    devolvido não é normalizado (tipos são preservados) — a normalização de
    ``cacheMode``/``optimize`` permanece no builder, evitando regras conflitantes.
    """
    prefix = f"{path}: " if path is not None else ""
    if not isinstance(data, dict):
        raise ConfigError(
            f"{prefix}deve conter um objeto JSON (ex.: {{ \"incremental\": true }}), "
            f"encontrado {_type_name(data)}."
        )

    errors: list[str] = []
    for key in data:
        if not isinstance(key, str):
            errors.append(f"chave {key!r} deve ser um texto; encontrado {_type_name(key)}.")
            continue
        if key not in FIELDS:
            suggestion = difflib.get_close_matches(key, FIELDS.keys(), n=1, cutoff=0.6)
            hint = f" Você quis dizer '{suggestion[0]}'?" if suggestion else ""
            known = ", ".join(sorted(FIELDS))
            errors.append(f"chave desconhecida '{key}'.{hint} Chaves válidas: {known}.")

    for key, (check, expected) in FIELDS.items():
        if key not in data:
            continue
        value = data[key]
        if not check(value):
            errors.append(
                f"'{key}' deve ser {expected}; "
                f"encontrado {value!r} ({_type_name(value)})."
            )

    if errors:
        raise ConfigError(prefix + " ".join(errors))
    return dict(data)


def load(path: Path | str) -> dict:
    """Lê e valida ``apkfast.json``. Arquivo ausente equivale a config vazia."""
    config_path = Path(path)
    if not config_path.is_file():
        return {}
    try:
        data = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, ValueError) as exc:
        raise ConfigError(f"{config_path}: JSON inválido: {exc}") from exc
    return validate(data, path=config_path)


def effective(config: dict) -> dict:
    """Configuração efetiva (defaults + projeto) para fins de inspeção."""
    result = dict(DEFAULTS)
    result.update(config)
    return result
