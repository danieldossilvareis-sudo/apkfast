"""Cache de recursos, DEX (D8/R8), APK (unsigned/signed) e AAB assinado.

Este módulo complementa :mod:`apkfast.cache` (que cobre objetos C/C++ e link da
``.so``). Ele reaproveita exatamente a mesma raiz de cache, os blobs por SHA256,
os locks por ``(projectId, ABI)`` e o modo ``trusted``/``verify``.

As chaves são por *conteúdo de ABI*, não pela lista de ABIs escolhida na linha
de comando: as ``.so`` entram como um mapa ``<abi> -> hash``. Assim uma ABI
isolada e a mesma ABI dentro de ``--abi all`` produzem a mesma chave quando o
conteúdo relevante é idêntico.

Nada aqui grava senhas. A identidade da keystore entra nas chaves do APK e do
AAB assinados através de ``apkfast.signing.signature_fingerprint()`` quando o
módulo está disponível; se não estiver, usa-se o SHA256 do arquivo da keystore
(fallback local, nunca uma chave vazia quando o arquivo existe).
"""

from __future__ import annotations

import io
import json
import os
import re
import shutil
import subprocess
import uuid
import zipfile
from functools import lru_cache
from pathlib import Path
from typing import Any, Iterable

from . import cache as cache_mod

CACHE_SCHEMA = 1
ENTRY_RESOURCE = "resource"
ENTRY_DEX = "dex"
ENTRY_APK_LINK = "apkLink"
ENTRY_APK = "apk"
ENTRY_AAB = "aab"

_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_IGNORED_DIRS = frozenset(
    {
        ".git", ".apkfast", ".apkfast-cache", "build", "obj",
        "__pycache__", "gradle", ".gradle",
    }
)
# Mesmo conjunto usado por builder._iter_files() para descobrir .java/.jar.
_JAVA_IGNORED_DIRS = frozenset(_IGNORED_DIRS | {"gradle", ".gradle"})
# zipalign é chamado com -P 16 no builder; se a ferramenta não aceitar, o
# builder cai para 4. O valor faz parte da chave para documentar a intenção.
ZIPALIGN_PAGES = 16


class ResourceCacheError(RuntimeError):
    """Falha de configuração/gravação do cache de recursos/APK."""


class CacheMiss(ResourceCacheError):
    """Entrada ausente, inválida ou incompatível (o build deve continuar)."""


def cache_event(stage: str, result: str, reason: str, key: str = "") -> dict:
    """Evento padronizado para ``cache_events`` do relatório de build."""
    return {
        "stage": str(stage),
        "result": "hit" if result == "hit" else "miss",
        "reason": str(reason)[:200],
        "key": str(key)[:12],
    }


# ---------------------------------------------------------------------------
# Chaves
# ---------------------------------------------------------------------------


def _roots(project: Path, tc: Any, ctx: cache_mod.CacheContext) -> list[tuple[str, str]]:
    apkfast_root = Path(__file__).resolve().parent.parent
    try:
        return cache_mod.make_roots(
            project=project,
            apkfast_root=apkfast_root,
            tc=tc,
            work=None,
            cache_root=ctx.root,
        )
    except (AttributeError, OSError, TypeError):
        pairs = [
            (str(ctx.root.resolve()), "<cache>"),
            (str(apkfast_root.resolve()), "<apkfast>"),
            (str(project.resolve()), "<project>"),
        ]
        return sorted(pairs, key=lambda pair: len(pair[0]), reverse=True)


def _tree_state(root: Path) -> list[list[str]]:
    """Estado portável de um diretório: [[caminho relativo, sha256], ...]."""
    if not root.is_dir():
        return []
    state: list[list[str]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        relative = path.relative_to(root)
        parts = relative.parts
        if any(part in _IGNORED_DIRS or part.startswith(".") for part in parts):
            continue
        state.append([relative.as_posix(), cache_mod.sha256_file(path)])
    return state


def aapt2_version(tc: Any) -> str:
    """Saída curta de ``aapt2 --version`` (vazia se indisponível)."""
    tool = getattr(tc, "aapt2", None)
    if not tool:
        return ""
    try:
        result = subprocess.run(
            [str(tool), "--version"],
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    text = f"{result.stdout or ''}\n{result.stderr or ''}"
    return " | ".join(line.strip() for line in text.splitlines() if line.strip())[:512]


def resource_key(
    project: Path,
    manifest: Any,
    options: Any,
    tc: Any,
    cache_ctx: cache_mod.CacheContext,
) -> str:
    """Chave dos flats do aapt2.

    Invalida quando mudam: ``res/**``, ``assets/**`` (inclusive --assets),
    AndroidManifest.xml, minSdk/targetSdk, package, versão do app, a versão do
    aapt2 (caminho lógico + ``--version``) ou o namespace/projectId do cache.
    """
    project = Path(project).expanduser().resolve()
    roots = _roots(project, tc, cache_ctx)
    manifest_path = project / "AndroidManifest.xml"

    assets_raw = getattr(options, "assets_dir", None) or (project / "assets")
    assets = Path(assets_raw).expanduser()
    if not assets.is_absolute():
        assets = project / assets
    assets = assets.resolve()

    aapt2 = Path(str(getattr(tc, "aapt2", "") or ""))
    payload = {
        "schema": CACHE_SCHEMA,
        "kind": "resource",
        # projectId já embute package + lib + namespace do cache. Repetimos o
        # namespace para deixar explícito o invalidador pedido pelo contrato.
        "projectId": cache_ctx.project_id,
        "namespace": str(getattr(options, "cache_namespace", "") or ""),
        "package": str(getattr(manifest, "package", "") or ""),
        "versionCode": str(getattr(manifest, "version_code", "") or ""),
        "versionName": str(getattr(manifest, "version_name", "") or ""),
        "minSdk": int(getattr(options, "min_sdk", 0) or 0),
        "targetSdk": int(getattr(options, "target_sdk", 0) or 0),
        "manifest": (
            cache_mod.sha256_file(manifest_path) if manifest_path.is_file() else ""
        ),
        "res": _tree_state(project / "res"),
        "assetsRoot": cache_mod.logical_path(assets, roots),
        "assets": _tree_state(assets),
        "aapt2": {
            "path": cache_mod.logical_path(aapt2, roots),
            "version": aapt2_version(tc),
        },
    }
    return cache_mod.sha256_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":"))
    )


def _java_state(project: Path) -> list[list[str]]:
    """Fontes Java/JAR do projeto como ``[[caminho relativo, sha256], ...]``.

    Usa o mesmo conjunto de diretórios ignorados do builder para que a chave
    mude exatamente quando a entrada real de ``javac``/D8/R8 muda.
    """
    project = Path(project)
    state: list[list[str]] = []
    for dirpath, dirnames, filenames in os.walk(project):
        dirnames[:] = sorted(
            name for name in dirnames if name not in _JAVA_IGNORED_DIRS
        )
        for name in filenames:
            if not name.endswith((".java", ".jar")):
                continue
            path = Path(dirpath) / name
            try:
                relative = path.relative_to(project).as_posix()
                digest = cache_mod.sha256_file(path)
            except (OSError, ValueError):
                continue
            state.append([relative, digest])
    return sorted(state, key=lambda item: item[0])


def _manifest_java_config(manifest: Any) -> dict:
    """Parte do manifesto que influencia classes Java/keep rules do R8.

    O manifesto cru já entra em ``resource_key``; aqui entram apenas nomes de
    classes e flags de aplicação/atividade, para que mudar um rótulo de recursos
    não invalide o DEX desnecessariamente.
    """
    relevant = {
        "name", "android:name", "process", "android:process",
        "debuggable", "android:debuggable", "hasCode", "android:hasCode",
        "persistent", "android:persistent", "exported", "android:exported",
        "launchMode", "android:launchMode", "permission", "android:permission",
    }
    application = getattr(manifest, "application_attrs", None) or {}
    app_config = {
        str(key): str(value)
        for key, value in dict(application).items()
        if str(key) in relevant
    }
    activities = []
    for activity in sorted(
        getattr(manifest, "activities", None) or [], key=lambda item: str(item.name)
    ):
        attrs = dict(getattr(activity, "attributes", None) or {})
        activities.append(
            {
                "name": str(getattr(activity, "name", "") or ""),
                "attrs": {
                    str(key): str(value)
                    for key, value in attrs.items()
                    if str(key) in relevant
                },
            }
        )
    return {
        "package": str(getattr(manifest, "package", "") or ""),
        "application": app_config,
        "activities": activities,
    }


def java_tool_version(tool: Path | str | None) -> str:
    """Saída curta de ``javac/d8/r8 --version`` (vazia se indisponível)."""
    if not tool:
        return ""
    try:
        result = subprocess.run(
            [str(tool), "--version"],
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    text = f"{result.stdout or ''}\n{result.stderr or ''}"
    return " | ".join(line.strip() for line in text.splitlines() if line.strip())[:512]


def _java_tool_path(tc: Any, name: str) -> Path | None:
    """Resolve ferramentas Java nos mesmos lugares usados pelo builder."""
    explicit = getattr(tc, name, None)
    if explicit:
        candidate = Path(str(explicit))
        if candidate.is_file():
            return candidate
    sdk_dir = getattr(tc, "sdk_dir", None)
    build_tools = getattr(tc, "build_tools_dir", None)
    candidates = []
    if sdk_dir:
        candidates.append(Path(sdk_dir) / "cmdline-tools" / "bin" / name)
    if build_tools:
        candidates += [Path(build_tools) / name, Path(build_tools) / "lib" / name]
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def dex_key(
    project: Path,
    manifest: Any,
    options: Any,
    tc: Any,
    cache_ctx: cache_mod.CacheContext,
    *,
    d8: Path | str | None = None,
    r8: Path | str | None = None,
    javac: Path | str | None = None,
) -> str:
    """Chave do DEX gerado por javac + D8/R8.

    Invalida quando mudam: fontes ``.java``/``.jar`` do projeto, ``optimize``,
    ``obfuscate``, ``minSdk``, a versão/identidade de ``javac``, ``d8``/``r8``,
    as regras do R8 e a configuração Java do manifesto. As ABIs não entram:
    o DEX é compartilhado entre builds de uma ABI e ``--abi all``.
    """
    project = Path(project).expanduser().resolve()
    roots = _roots(project, tc, cache_ctx)
    tools = {
        "javac": javac if javac is not None else _java_tool_path(tc, "javac"),
        "d8": d8 if d8 is not None else _java_tool_path(tc, "d8"),
        "r8": r8 if r8 is not None else _java_tool_path(tc, "r8"),
    }
    tool_state = {}
    for name, tool in tools.items():
        tool_state[name] = {
            "path": cache_mod.logical_path(Path(str(tool)), roots) if tool else "",
            "version": java_tool_version(tool),
            "sha256": _tool_identity(tool),
        }

    rules = project / "proguard-rules.pro"
    payload = {
        "schema": CACHE_SCHEMA,
        "kind": "dex",
        "projectId": cache_ctx.project_id,
        "namespace": str(getattr(options, "cache_namespace", "") or ""),
        "sources": _java_state(project),
        "optimize": str(getattr(options, "optimize", "") or ""),
        "obfuscate": bool(getattr(options, "obfuscate", False)),
        "minSdk": int(getattr(options, "min_sdk", 0) or 0),
        "javaConfig": _manifest_java_config(manifest),
        "pgConf": (
            cache_mod.sha256_file(rules) if rules.is_file() else ""
        ),
        "androidJar": cache_mod.logical_path(
            Path(str(getattr(tc, "platform_dir", "") or "")), roots
        ),
        "tools": tool_state,
    }
    return cache_mod.sha256_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":"))
    )


def keystore_fingerprint_info(keystore: Path | str | None) -> dict:
    """Fingerprint da keystore e origem do cálculo (sem senhas)."""
    if keystore is None:
        return {
            "fingerprint": "",
            "source": "missing",
            "available": False,
        }
    path = Path(keystore).expanduser()
    try:
        from . import signing  # import tolerante: módulo pode ainda não existir
    except ImportError:
        signing = None  # type: ignore[assignment]
    function = getattr(signing, "signature_fingerprint", None)
    if callable(function):
        try:
            value = function(path)
            text = str(value).strip().lower().replace(":", "")
            if text:
                return {
                    "fingerprint": text,
                    "source": "apkfast.signing.signature_fingerprint",
                    "available": True,
                }
        except (OSError, RuntimeError, ValueError, TypeError):
            pass  # cai para o digest do arquivo, ainda ligado à chave
    try:
        digest = cache_mod.sha256_file(path)
    except OSError:
        digest = ""
    return {
        "fingerprint": digest,
        "source": "sha256(keystore-file)" if digest else "unavailable",
        "available": False,
    }


@lru_cache(maxsize=32)
def _hashed_tool(path_text: str, size: int, mtime_ns: int) -> str:
    try:
        return cache_mod.sha256_file(Path(path_text))
    except OSError:
        return ""


def _tool_identity(tool: Any) -> str:
    if not tool:
        return ""
    path = Path(str(tool))
    try:
        stat = path.stat()
    except OSError:
        return ""
    return _hashed_tool(str(path), stat.st_size, stat.st_mtime_ns)


def native_libs_state(
    apk_root: Path, *, abis: Iterable[str] | None = None
) -> dict[str, list[list[str]]]:
    """Mapa ``<abi> -> [[arquivo, sha256], ...]`` das ``.so`` do work dir.

    A lista de ABIs escolhida na CLI nunca é concatenada na chave: ela serve
    apenas para selecionar quais entradas do mapa entram nela. Com ``abis=None``
    usa-se todas as ABIs encontradas em ``lib/``.
    """
    lib_root = Path(apk_root) / "lib"
    state: dict[str, list[list[str]]] = {}
    if lib_root.is_dir():
        for abi_dir in sorted(lib_root.iterdir()):
            if not abi_dir.is_dir() or abi_dir.is_symlink():
                continue
            files: list[list[str]] = []
            for path in sorted(abi_dir.rglob("*.so")):
                if not path.is_file() or path.is_symlink():
                    continue
                files.append(
                    [
                        path.relative_to(abi_dir).as_posix(),
                        cache_mod.sha256_file(path),
                    ]
                )
            state[abi_dir.name] = files
    if abis is None:
        return state
    return {abi: state.get(abi, []) for abi in sorted(set(abis))}


def apk_key(
    project: Path,
    manifest: Any,
    options: Any,
    tc: Any,
    cache_ctx: cache_mod.CacheContext,
    *,
    apk_root: Path,
    lib_name: str,
    keystore: Path | str | None = None,
    abis: Iterable[str] | None = None,
    signed: bool = True,
) -> str:
    """Chave do APK.

    ``signed=True`` (padrão) inclui resource_key, o mapa ``<abi> -> hash`` das
    ``.so``, os hashes das ``.dex`` de ``apk_root``, nome da lib, flags de
    zipalign/assinatura e o fingerprint da keystore. ``signed=False`` gera a
    chave do APK *unsigned* posterior ao aapt2 link, sem dados de assinatura.

    ``abis`` seleciona entradas do mapa de libs; não é uma "combinação de ABIs"
    na chave. O mesmo conteúdo de uma ABI gera a mesma chave em ``--abi X`` e em
    ``--abi all`` (quando ``abis`` filtra apenas aquela ABI).
    """
    apk_root = Path(apk_root)
    dex_state = [
        item for item in _tree_state(apk_root) if item[0].endswith(".dex")
    ]
    payload: dict[str, Any] = {
        "schema": CACHE_SCHEMA,
        "kind": "signed-apk" if signed else "unsigned-apk",
        "resource": resource_key(project, manifest, options, tc, cache_ctx),
        "natives": native_libs_state(apk_root, abis=abis),
        "lib": str(lib_name),
        "release": bool(getattr(options, "release", False)),
        "obfuscate": bool(getattr(options, "obfuscate", False)),
        "dexFiles": dex_state,
    }
    if signed:
        info = keystore_fingerprint_info(keystore)
        payload["signing"] = {
            "keystore": info["fingerprint"],
            "fingerprintSource": info["source"],
            "keyAlias": str(getattr(options, "key_alias", "") or ""),
            "v1": int(getattr(options, "min_sdk", 21) or 21) < 24,
            "v2": True,
            "zipalign": _tool_identity(getattr(tc, "zipalign", None)),
            "zipalignPages": ZIPALIGN_PAGES,
            "apksigner": _tool_identity(getattr(tc, "apksigner", None)),
        }
    return cache_mod.sha256_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":"))
    )


def aab_key(
    project: Path,
    manifest: Any,
    options: Any,
    tc: Any,
    cache_ctx: cache_mod.CacheContext,
    *,
    apk_root: Path,
    lib_name: str,
    keystore: Path | str | None,
    abis: Iterable[str] | None = None,
    bundletool_version: str | None = None,
    precomputed_dex_key: str | None = None,
) -> str:
    """Chave do AAB **assinado**.

    Combina resource_key + dex_key + mapa ``<abi> -> hash`` das ``.so`` +
    versão do bundletool + fingerprint da keystore + flags de assinatura AAB
    (alias, algoritmo e identidade do jarsigner). Senhas nunca entram na chave.

    Trocar a keystore (ou o bundletool) muda a chave: um AAB assinado com outra
    chave nunca é servido. ``precomputed_dex_key`` evita recomputar o DEX quando
    o builder já calculou a chave na etapa anterior.
    """
    from . import bundle as bundle_mod  # import local: módulo é leve e sem ciclo

    version = str(
        bundletool_version
        or getattr(options, "bundletool_version", "")
        or bundle_mod.BUNDLETOOL_VERSION
    )
    info = keystore_fingerprint_info(keystore)
    jarsigner = shutil.which("jarsigner") or ""
    payload: dict[str, Any] = {
        "schema": CACHE_SCHEMA,
        "kind": "signed-aab",
        "projectId": cache_ctx.project_id,
        "namespace": str(getattr(options, "cache_namespace", "") or ""),
        "resource": resource_key(project, manifest, options, tc, cache_ctx),
        "dex": precomputed_dex_key
        or dex_key(project, manifest, options, tc, cache_ctx),
        "natives": native_libs_state(apk_root, abis=abis),
        "lib": str(lib_name),
        "bundletool": {"version": version},
        "signing": {
            "keystore": info["fingerprint"],
            "fingerprintSource": info["source"],
            "keyAlias": str(getattr(options, "key_alias", "") or ""),
            "scheme": "v1-jar",
            "digestAlg": "SHA-256",
            "jarsigner": _tool_identity(jarsigner),
        },
    }
    return cache_mod.sha256_text(
        json.dumps(payload, sort_keys=True, separators=(",", ":"))
    )


# ---------------------------------------------------------------------------
# Blobs de recursos / DEX / APK / AAB
# ---------------------------------------------------------------------------


def _read_entry(
    ctx: cache_mod.CacheContext, name: str, key: str
) -> dict | None:
    if not key:
        return None
    for abi in ctx.abis:
        entry = cache_mod.read_cache(ctx, abi).get(name)
        if isinstance(entry, dict) and entry.get("key") == key:
            return entry
    return None


def apk_record(
    ctx: cache_mod.CacheContext, key: str, *, signed: bool = True
) -> dict | None:
    """Entrada armazenada para uma chave de APK (útil no diagnóstico de miss)."""
    return _read_entry(ctx, ENTRY_APK if signed else ENTRY_APK_LINK, key)


def apk_records(
    ctx: cache_mod.CacheContext, *, signed: bool = True
) -> list[dict]:
    """Todas as entradas de APK deste projeto (para explicar misses)."""
    name = ENTRY_APK if signed else ENTRY_APK_LINK
    records: list[dict] = []
    for abi in ctx.abis:
        entry = cache_mod.read_cache(ctx, abi).get(name)
        if isinstance(entry, dict):
            records.append(entry)
    return records


def _write_entry(ctx: cache_mod.CacheContext, name: str, record: dict) -> None:
    if not ctx.writable:
        return
    for abi in ctx.abis:
        data = cache_mod.read_cache(ctx, abi)
        data[name] = record
        cache_mod.write_cache(ctx, abi, data)


def _valid_blob(
    ctx: cache_mod.CacheContext, digest: Any, *, require_zip: bool = True
) -> Path:
    """Valida SHA256/tamanho do blob; DEX não é ZIP e por isso é dispensado."""
    if not isinstance(digest, str) or not _HEX64.fullmatch(digest):
        raise CacheMiss("hash de blob inválido")
    abi = ctx.abis[0] if ctx.abis else ""
    try:
        path = cache_mod.ensure_blob(ctx, digest, abi=abi, check_elf=False)
        if require_zip and not zipfile.is_zipfile(path):
            raise CacheMiss("blob não é um ZIP válido")
        return path
    except (cache_mod.CacheError, OSError, zipfile.BadZipFile) as exc:
        raise CacheMiss(f"blob inválido: {exc}") from exc


def _check_dex(path: Path) -> Path:
    try:
        with path.open("rb") as stream:
            header = stream.read(8)
    except OSError as exc:
        raise CacheMiss(f"falha ao ler DEX: {exc}") from exc
    if not header.startswith(b"dex\n"):
        raise CacheMiss("blob não é um DEX válido")
    return path


def _safe_member(name: str) -> bool:
    return bool(name) and not (
        name.startswith("/")
        or "\\" in name
        or ".." in Path(name).parts
        or Path(name).is_absolute()
    )


def store_resource(
    ctx: cache_mod.CacheContext,
    key: str,
    flats: Iterable[Path],
) -> str | None:
    """Grava todos os ``.flat`` num único blob ZIP. Devolve o digest ou None."""
    if not key or not ctx.writable:
        return None
    stream = io.BytesIO()
    files: list[dict] = []
    try:
        with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(set(flats)):
                if not path.is_file():
                    continue
                name = path.name
                if not _safe_member(name):
                    continue
                data = path.read_bytes()
                archive.writestr(name, data)
                files.append(
                    {
                        "name": name,
                        "sha256": cache_mod.sha256_bytes(data),
                        "size": len(data),
                    }
                )
        digest = cache_mod.write_blob(ctx, stream.getvalue())
    except (OSError, cache_mod.CacheError, zipfile.BadZipFile):
        return None
    _write_entry(
        ctx,
        ENTRY_RESOURCE,
        {
            "version": CACHE_SCHEMA,
            "key": key,
            "blob": digest,
            "files": files,
        },
    )
    return digest


def load_resource(
    ctx: cache_mod.CacheContext, key: str, destination: Path
) -> list[Path]:
    """Copia os flats do blob para ``destination`` ou levanta :class:`CacheMiss`."""
    record = _read_entry(ctx, ENTRY_RESOURCE, key)
    if record is None:
        raise CacheMiss("sem entrada de recursos para esta chave")
    source = _valid_blob(ctx, record.get("blob"))
    files = record.get("files")
    if not isinstance(files, list) or not files:
        raise CacheMiss("entrada de recursos sem arquivos")
    payloads: dict[str, bytes] = {}
    try:
        with zipfile.ZipFile(source) as archive:
            for item in files:
                if not isinstance(item, dict):
                    raise CacheMiss("entrada de recursos malformada")
                name = str(item.get("name", ""))
                if not _safe_member(name):
                    raise CacheMiss("nome de flat inseguro no blob")
                data = archive.read(name)
                if cache_mod.sha256_bytes(data) != item.get("sha256"):
                    raise CacheMiss("flat do blob divergente do manifesto")
                payloads[name] = data
    except (KeyError, OSError, zipfile.BadZipFile) as exc:
        raise CacheMiss(f"falha ao ler flats: {exc}") from exc

    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    result: list[Path] = []
    for name, data in payloads.items():
        target = destination / name
        target.write_bytes(data)
        result.append(target)
    return sorted(result)


def _store_single(
    ctx: cache_mod.CacheContext,
    key: str,
    name: str,
    path: Path,
    *,
    metadata: dict | None = None,
    check: Any = None,
) -> str | None:
    """Grava um arquivo único (DEX/AAB) como blob e devolve o digest."""
    if not key or not ctx.writable:
        return None
    try:
        data = Path(path).read_bytes()
        if not data or len(data) > cache_mod.MAX_BLOB_BYTES:
            return None
        if check is not None and not check(data):
            return None
        digest = cache_mod.write_blob(ctx, data)
    except (OSError, cache_mod.CacheError):
        return None
    _write_entry(
        ctx,
        name,
        {
            "version": CACHE_SCHEMA,
            "key": key,
            "blob": digest,
            "size": len(data),
            "metadata": dict(metadata or {}),
        },
    )
    return digest


def _copy_blob(
    ctx: cache_mod.CacheContext,
    key: str,
    name: str,
    destination: Path,
    *,
    require_zip: bool = False,
    source_check: Any = None,
) -> Path:
    record = _read_entry(ctx, name, key)
    if record is None:
        raise CacheMiss(f"sem entrada para esta chave ({name})")
    source = _valid_blob(ctx, record.get("blob"), require_zip=require_zip)
    if source_check is not None:
        source = source_check(source)
    expected = record.get("size")
    if isinstance(expected, int) and source.stat().st_size != expected:
        raise CacheMiss("tamanho do blob divergente do manifesto")
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    tmp = destination.with_name(destination.name + f".tmp-{uuid.uuid4().hex}")
    try:
        shutil.copyfile(source, tmp)
        os.replace(tmp, destination)
    except OSError as exc:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise CacheMiss(f"falha ao copiar blob: {exc}") from exc
    return destination


def store_dex(ctx: cache_mod.CacheContext, key: str, dex: Path) -> str | None:
    """Grava um ``classes.dex`` como blob (não é ZIP)."""
    return _store_single(
        ctx, key, ENTRY_DEX, dex, check=lambda data: data.startswith(b"dex\n")
    )


def load_dex(ctx: cache_mod.CacheContext, key: str, destination: Path) -> Path:
    """Copia o DEX do blob para ``destination`` ou levanta :class:`CacheMiss`."""
    return _copy_blob(ctx, key, ENTRY_DEX, destination, source_check=_check_dex)


def aab_record(ctx: cache_mod.CacheContext, key: str) -> dict | None:
    """Entrada armazenada para uma chave de AAB (diagnóstico de miss)."""
    return _read_entry(ctx, ENTRY_AAB, key)


def aab_records(ctx: cache_mod.CacheContext) -> list[dict]:
    """Todas as entradas de AAB deste projeto (para explicar misses)."""
    records: list[dict] = []
    for abi in ctx.abis:
        entry = cache_mod.read_cache(ctx, abi).get(ENTRY_AAB)
        if isinstance(entry, dict):
            records.append(entry)
    return records


def store_aab(
    ctx: cache_mod.CacheContext,
    key: str,
    aab: Path,
    *,
    metadata: dict | None = None,
) -> str | None:
    """Grava o AAB assinado como blob. Devolve o digest ou None."""
    return _store_single(
        ctx,
        key,
        ENTRY_AAB,
        aab,
        metadata=metadata,
        check=lambda data: zipfile.is_zipfile(io.BytesIO(data)),
    )


def load_aab(ctx: cache_mod.CacheContext, key: str, destination: Path) -> Path:
    """Copia o AAB assinado do blob para o destino final."""
    return _copy_blob(ctx, key, ENTRY_AAB, destination, require_zip=True)


def store_apk(
    ctx: cache_mod.CacheContext,
    key: str,
    apk: Path,
    *,
    unsigned: bool = False,
    metadata: dict | None = None,
) -> str | None:
    """Grava o APK assinado (ou o unsigned) como blob. Devolve o digest ou None."""
    if not key or not ctx.writable:
        return None
    apk = Path(apk)
    try:
        data = apk.read_bytes()
        if not data or len(data) > cache_mod.MAX_BLOB_BYTES:
            return None
        digest = cache_mod.write_blob(ctx, data)
    except (OSError, cache_mod.CacheError):
        return None
    _write_entry(
        ctx,
        ENTRY_APK_LINK if unsigned else ENTRY_APK,
        {
            "version": CACHE_SCHEMA,
            "key": key,
            "blob": digest,
            "size": len(data),
            "metadata": dict(metadata or {}),
        },
    )
    return digest


def load_apk(
    ctx: cache_mod.CacheContext,
    key: str,
    destination: Path,
    *,
    unsigned: bool = False,
) -> Path:
    """Copia o APK do blob para ``destination`` ou levanta :class:`CacheMiss`."""
    name = ENTRY_APK_LINK if unsigned else ENTRY_APK
    record = _read_entry(ctx, name, key)
    if record is None:
        raise CacheMiss("sem entrada de APK para esta chave")
    source = _valid_blob(ctx, record.get("blob"))
    expected = record.get("size")
    if isinstance(expected, int) and source.stat().st_size != expected:
        raise CacheMiss("tamanho do APK divergente do manifesto")
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    tmp = destination.with_name(destination.name + f".tmp-{uuid.uuid4().hex}")
    try:
        shutil.copyfile(source, tmp)
        os.replace(tmp, destination)
    except OSError as exc:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise CacheMiss(f"falha ao copiar APK: {exc}") from exc
    return destination


# ---------------------------------------------------------------------------
# Limpeza escopada por projeto
# ---------------------------------------------------------------------------


def _referenced_digests(ctx: cache_mod.CacheContext) -> set[str]:
    """Todos os blobs referenciados por qualquer cache.json sob a raiz."""
    referenced: set[str] = set()

    def walk(value: Any) -> None:
        if isinstance(value, dict):
            for key, item in value.items():
                if "blob" in str(key) and isinstance(item, str) and _HEX64.fullmatch(item):
                    referenced.add(item)
                else:
                    walk(item)
        elif isinstance(value, list):
            for item in value:
                walk(item)

    for cache_file in ctx.root.glob("*/**/cache.json"):
        if ctx.root / ".locks" in cache_file.parents:
            continue
        try:
            walk(json.loads(cache_file.read_text()))
        except (OSError, ValueError):
            continue
    return referenced


def _clear_entries(ctx: cache_mod.CacheContext, names: tuple[str, ...]) -> None:
    removed: set[str] = set()
    for abi in ctx.abis:
        data = cache_mod.read_cache(ctx, abi)
        changed = False
        for name in names:
            entry = data.pop(name, None)
            if isinstance(entry, dict):
                digest = entry.get("blob")
                if isinstance(digest, str) and _HEX64.fullmatch(digest):
                    removed.add(digest)
                changed = True
        if changed and ctx.writable:
            cache_mod.write_cache(ctx, abi, data)
    if not removed:
        return
    # Só apaga blobs que nenhum outro projeto (nem outra entrada) referencia.
    still_used = _referenced_digests(ctx)
    for digest in removed - still_used:
        try:
            ctx.blob_path(digest).unlink()
        except OSError:
            pass


def clear_resource_cache(ctx: cache_mod.CacheContext) -> None:
    """Remove apenas as entradas de recursos deste projeto/namespace."""
    _clear_entries(ctx, (ENTRY_RESOURCE,))


def clear_apk_cache(ctx: cache_mod.CacheContext) -> None:
    """Remove apenas as entradas de APK deste projeto/namespace."""
    _clear_entries(ctx, (ENTRY_APK_LINK, ENTRY_APK))


def clear_dex_cache(ctx: cache_mod.CacheContext) -> None:
    """Remove apenas as entradas de DEX deste projeto/namespace."""
    _clear_entries(ctx, (ENTRY_DEX,))


def clear_aab_cache(ctx: cache_mod.CacheContext) -> None:
    """Remove apenas as entradas de AAB deste projeto/namespace."""
    _clear_entries(ctx, (ENTRY_AAB,))


def clear_all_resource_caches(ctx: cache_mod.CacheContext) -> None:
    """Remove resource/dex/APK/AAB (usado por ``--clean-cache``)."""
    _clear_entries(
        ctx, (ENTRY_RESOURCE, ENTRY_DEX, ENTRY_APK_LINK, ENTRY_APK, ENTRY_AAB)
    )
