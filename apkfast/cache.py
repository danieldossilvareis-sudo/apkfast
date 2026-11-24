"""Cache incremental portável, compartilhável e verificável.

O schema 2 substitui o schema 1 (v2.2.0), que usava caminhos absolutos nas
chaves e não podia ser reaproveitado entre checkouts/máquinas.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import shutil
import socket
import subprocess
import tarfile
import tempfile
import time
import uuid
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Callable, Iterable

CACHE_SCHEMA = 2
BUNDLE_FORMAT = "apkfast-cache-bundle"
BUNDLE_VERSION = 1
MAX_BLOB_BYTES = 512 * 1024 * 1024
LOCK_TTL_SECONDS = 300
# Entradas de resource/dex/APK/AAB (ver apkfast.resource_cache). São aditivas
# ao manifesto do bundle: bundles antigos não as têm e continuam importáveis.
BUNDLE_ENTRIES = ("resource", "dex", "apkLink", "apk", "aab")

_ELF_MACHINES = {
    "arm64-v8a": 0xB7,     # EM_AARCH64
    "armeabi-v7a": 0x28,   # EM_ARM
    "x86": 0x03,           # EM_386
    "x86_64": 0x3E,        # EM_X86_64
}

_BLOB_NAME = re.compile(r"^[0-9a-f]{64}$")
_BLOB_DIR = re.compile(r"^[0-9a-f]{2}$")


class CacheError(RuntimeError):
    pass


@dataclass
class CacheContext:
    root: Path
    project_id: str
    mode: str                 # trusted | verify
    require_signature: bool
    abis: tuple[str, ...]
    writable: bool = True

    def abi_dir(self, abi: str) -> Path:
        return self.root / self.project_id / abi

    def cache_file(self, abi: str) -> Path:
        return self.abi_dir(abi) / "cache.json"

    def lock_dir(self, abi: str) -> Path:
        return self.root / ".locks" / self.project_id / f"{abi}.lockdir"

    def blob_path(self, digest: str) -> Path:
        return self.root / "blobs" / digest[:2] / digest

    def depfile(self, abi: str, key: str) -> Path:
        return self.abi_dir(abi) / f"dep_{key[:20]}.d"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8", "replace")).hexdigest()


def project_id(package: str, lib_name: str, namespace: str = "") -> str:
    payload = json.dumps(
        {"package": package, "lib": lib_name, "namespace": namespace},
        sort_keys=True,
        separators=(",", ":"),
    )
    return sha256_text(payload)


def default_root() -> Path:
    xdg = os.environ.get("XDG_CACHE_HOME")
    base = Path(xdg).expanduser() if xdg else Path.home() / ".cache"
    return base / "apkfast"


def resolve_context(
    project: Path,
    *,
    package: str,
    lib_name: str,
    abis: Iterable[str],
    cache_dir: Path | None = None,
    cache_mode: str | None = None,
    namespace: str = "",
    require_signature: bool = False,
) -> CacheContext:
    """Resolve a raiz/modo do cache. CLI e JSON já foram aplicados pelo chamador."""
    explicit = cache_dir is not None
    if cache_dir is None:
        root = default_root()
    else:
        root = cache_dir.expanduser()
        if not root.is_absolute():
            root = project / root
        root = root.resolve()

    mode = (cache_mode or ("verify" if explicit else "trusted")).lower()
    if mode not in ("trusted", "verify"):
        raise CacheError(f"Modo de cache inválido: {mode} (use trusted ou verify)")

    ctx = CacheContext(
        root=root,
        project_id=project_id(package, lib_name, namespace),
        mode=mode,
        require_signature=bool(require_signature),
        abis=tuple(abis),
    )
    ctx.writable = _probe_writable(root)
    return ctx


def _probe_writable(root: Path) -> bool:
    try:
        root.mkdir(parents=True, exist_ok=True)
        probe = root / f".probe-{uuid.uuid4().hex}"
        probe.write_bytes(b"")
        probe.unlink()
        return True
    except OSError:
        return False


def make_roots(
    *,
    project: Path,
    apkfast_root: Path,
    tc: object,
    work: Path | None = None,
    cache_root: Path | None = None,
) -> list[tuple[str, str]]:
    """Prefixos absolutos -> tokens lógicos (ordem: mais específico primeiro)."""
    pairs: list[tuple[str, str]] = []
    if cache_root is not None:
        pairs.append((str(cache_root.resolve()), "<cache>"))
    if work is not None:
        pairs.append((str(work.resolve()), "<work>"))
    glue = Path(tc.native_app_glue_dir()).resolve()  # type: ignore[attr-defined]
    pairs.append((str(glue), "<glue>"))
    sysroot = Path(tc.sysroot).resolve()  # type: ignore[attr-defined]
    pairs.append((str(sysroot), "<sysroot>"))
    ndk = Path(tc.ndk_dir).resolve()  # type: ignore[attr-defined]
    pairs.append((str(ndk), "<ndk>"))
    vendor = (apkfast_root / "vendor").resolve()
    pairs.append((str(vendor), "<vendor>"))
    pairs.append((str(apkfast_root.resolve()), "<apkfast>"))
    pairs.append((str(project.resolve()), "<project>"))
    # Ordem por tamanho evita <ndk> capturar <sysroot>, por exemplo.
    return sorted(pairs, key=lambda pair: len(pair[0]), reverse=True)


def logical_path(path: Path | str, roots: Iterable[tuple[str, str]]) -> str:
    raw = str(path)
    for prefix, token in roots:
        if raw == prefix:
            return token
        if raw.startswith(prefix + os.sep):
            return token + "/" + raw[len(prefix) + 1 :].replace(os.sep, "/")
    return "<external>/" + sha256_text(raw)[:32]


def canonical_flags(flags: Iterable[object], roots: Iterable[tuple[str, str]]) -> list[str]:
    roots = list(roots)
    out: list[str] = []
    for flag in flags:
        text = str(flag)
        if text.startswith("-I") and len(text) > 2:
            out.append("-I" + logical_path(text[2:], roots))
        elif text.startswith("--sysroot="):
            out.append("--sysroot=" + logical_path(text[len("--sysroot=") :], roots))
        elif text.startswith("-ffile-prefix-map=") or text.startswith("-fdebug-prefix-map="):
            prefix, value = text.split("=", 1)
            out.append(prefix + "=" + logical_path(value, roots))
        elif text.startswith("-fdebug-compilation-dir="):
            out.append(
                "-fdebug-compilation-dir="
                + logical_path(text[len("-fdebug-compilation-dir=") :], roots)
            )
        elif text in ("-target", "-isystem", "-include"):
            out.append(text)
        elif Path(text).is_absolute():
            # Inclui blobs sem extensão e o valor de -o.
            out.append(logical_path(text, roots))
        else:
            out.append(text)
    return out


def compiler_fingerprint(
    compiler: Path,
    *,
    abi: str,
    api: int,
    target_flags: Iterable[object],
    glue_source: Path,
    roots: Iterable[tuple[str, str]],
    run_version: Callable[[], str] | None = None,
) -> str:
    """Identidade da toolchain sem paths absolutos."""
    try:
        version = run_version() if run_version else subprocess.run(
            [str(compiler), "--version"], capture_output=True, text=True, timeout=30
        ).stdout
    except (OSError, subprocess.SubprocessError):
        version = ""
    # Normaliza números de build locais/versões impressas com paths.
    version_lines = [line.strip() for line in version.splitlines() if line.strip()]
    version_text = " | ".join(version_lines[:3])
    revision = ""
    ndk = Path(str(compiler)).parents
    for parent in ndk:
        properties = parent / "source.properties"
        if properties.is_file():
            try:
                revision = properties.read_text(errors="ignore")
            except OSError:
                revision = ""
            break
    revision_hash = sha256_text(revision[:4096])
    glue_hash = sha256_file(glue_source) if glue_source.is_file() else ""
    payload = json.dumps(
        {
            "basename": Path(compiler).name,
            "abi": abi,
            "api": api,
            "version": version_text,
            "revision": revision_hash,
            "glue": glue_hash,
            "target": canonical_flags(target_flags, roots),
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    return sha256_text(payload)


def dep_state(paths: Iterable[str], roots: Iterable[tuple[str, str]]) -> list[list[str]]:
    state = [[logical_path(raw, roots), sha256_file(Path(raw))] for raw in paths if Path(raw).is_file()]
    return sorted(state, key=lambda item: item[0])


def resolve_logical(logical: str, roots: Iterable[tuple[str, str]]) -> Path | None:
    """Inverte logical_path(): token + caminho relativo -> path absoluto atual."""
    for prefix, token in roots:
        if logical == token:
            return Path(prefix)
        marker = token + "/"
        if logical.startswith(marker):
            return Path(prefix) / logical[len(marker) :]
    return None


def verify_dep_state(state: Iterable[list[str]], roots: Iterable[tuple[str, str]]) -> list[list[str]]:
    """Recomputa hashes de um dep_state portátil no checkout atual."""
    result: list[list[str]] = []
    for item in state:
        if not isinstance(item, list) or len(item) != 2:
            return []
        logical, expected = item
        path = resolve_logical(logical, roots)
        if path is None or not path.is_file():
            return []
        result.append([logical, sha256_file(path)])
    return sorted(result, key=lambda pair: pair[0])


def parse_depfile(path: Path) -> list[str]:
    try:
        text = path.read_text(errors="ignore")
    except OSError:
        return []
    text = text.replace("\\\n", " ")
    if ":" not in text:
        return []
    body = text.split(":", 1)[1]
    tokens: list[str] = []
    current: list[str] = []
    index = 0
    while index < len(body):
        char = body[index]
        if char == "\\" and index + 1 < len(body):
            current.append(body[index + 1])
            index += 2
            continue
        if char.isspace():
            if current:
                tokens.append("".join(current))
                current = []
            index += 1
            continue
        current.append(char)
        index += 1
    if current:
        tokens.append("".join(current))
    return [token for token in tokens if Path(token).is_file()]


def object_key(
    *,
    source: Path,
    flags: Iterable[object],
    compiler_fp: str,
    api_key: str,
    abi: str,
    api: int,
    project_key: str,
    roots: Iterable[tuple[str, str]],
    extra_key: str = "",
) -> str:
    payload = json.dumps(
        {
            "src": logical_path(source, roots),
            "flags": canonical_flags(flags, roots),
            "compiler": compiler_fp,
            "api_headers": api_key,
            "abi": abi,
            "api": api,
            "project": project_key,
            "extra": extra_key,
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    return sha256_text(payload)


def link_key(
    *,
    command: Iterable[object],
    inputs: Iterable[Path],
    compiler_fp: str,
    strip_mode: str,
    lib_name: str,
    abi: str,
    project_key: str,
    roots: Iterable[tuple[str, str]],
) -> str:
    roots = list(roots)
    inputs_state = sorted(
        [logical_path(path, roots) + "|" + sha256_file(path) for path in inputs if path.is_file()]
    )
    payload = json.dumps(
        {
            "cmd": canonical_flags(command, roots),
            "inputs": inputs_state,
            "compiler": compiler_fp,
            "strip": strip_mode,
            "lib": lib_name,
            "abi": abi,
            "project": project_key,
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    return sha256_text(payload)


# ---------------------------------------------------------------------------
# Blobs e cache.json
# ---------------------------------------------------------------------------


def write_blob(ctx: CacheContext, data: bytes) -> str:
    digest = sha256_bytes(data)
    path = ctx.blob_path(digest)
    if path.is_file():
        return digest
    if not ctx.writable:
        raise CacheError("Cache é somente leitura; não é possível gravar blob")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp-{uuid.uuid4().hex}")
    try:
        tmp.write_bytes(data)
        os.replace(tmp, path)
    except OSError as exc:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise CacheError(f"Falha ao gravar blob {digest}: {exc}") from exc
    return digest


def read_blob(ctx: CacheContext, digest: str) -> bytes:
    path = ctx.blob_path(digest)
    if not path.is_file():
        raise CacheError(f"Blob ausente: {digest}")
    data = path.read_bytes()
    if sha256_bytes(data) != digest:
        raise CacheError(f"Blob corrompido: {digest}")
    return data


def ensure_blob(ctx: CacheContext, digest: str, *, abi: str, check_elf: bool = True) -> Path:
    """Valida um blob existente e devolve seu caminho."""
    if not isinstance(digest, str) or not _BLOB_NAME.fullmatch(digest):
        raise CacheError(f"Hash de blob inválido: {digest!r}")
    path = ctx.blob_path(digest)
    if not path.is_file():
        raise CacheError(f"Blob ausente: {digest}")
    if path.stat().st_size > MAX_BLOB_BYTES:
        raise CacheError(f"Blob grande demais: {digest}")
    if sha256_file(path) != digest:
        raise CacheError(f"Blob corrompido: {digest}")
    if ctx.mode == "verify" and check_elf and not elf_matches(path, abi):
        raise CacheError(f"ELF de {digest} não corresponde à ABI {abi}")
    return path


def elf_matches(path: Path, abi: str) -> bool:
    expected = _ELF_MACHINES.get(abi)
    if expected is None:
        return False
    try:
        with path.open("rb") as stream:
            header = stream.read(20)
    except OSError:
        return False
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return False
    # ELF64 (class 2) little-endian (1); arm64/x86_64, e_machine em 18..19.
    if header[4] == 2 and header[5] == 1:
        machine = int.from_bytes(header[18:20], "little")
        return machine == expected
    return False


def read_cache(ctx: CacheContext, abi: str) -> dict:
    path = ctx.cache_file(abi)
    try:
        data = json.loads(path.read_text())
    except (OSError, ValueError):
        return {"schema": CACHE_SCHEMA, "projectId": ctx.project_id, "abi": abi, "objects": {}}
    if not isinstance(data, dict) or data.get("schema") != CACHE_SCHEMA:
        return {"schema": CACHE_SCHEMA, "projectId": ctx.project_id, "abi": abi, "objects": {}}
    if data.get("projectId") != ctx.project_id or data.get("abi") != abi:
        return {"schema": CACHE_SCHEMA, "projectId": ctx.project_id, "abi": abi, "objects": {}}
    if not isinstance(data.get("objects"), dict):
        data["objects"] = {}
    return data


def write_cache(ctx: CacheContext, abi: str, cache: dict) -> None:
    if not ctx.writable:
        return
    path = ctx.cache_file(abi)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp-{uuid.uuid4().hex}")
    try:
        tmp.write_text(json.dumps(cache, sort_keys=True, separators=(",", ":")))
        os.replace(tmp, path)
    except OSError:
        try:
            tmp.unlink()
        except OSError:
            pass


def clear_abi(ctx: CacheContext, abi: str) -> None:
    path = ctx.abi_dir(abi)
    if path.is_dir():
        shutil.rmtree(path)


# ---------------------------------------------------------------------------
# Locks compartilhados (mkdir é confiável em NFS/sshfs)
# ---------------------------------------------------------------------------


@dataclass
class cache_lock:
    ctx: CacheContext
    abi: str
    path: Path
    token: str
    acquired: bool = False

    def __enter__(self) -> "cache_lock":
        acquire_lock(self)
        return self

    def __exit__(self, *_: object) -> None:
        release_lock(self)


def acquire_lock(lock: cache_lock) -> None:
    path = lock.path
    if not lock.ctx.writable:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    token = f"{socket.gethostname()}:{os.getpid()}:{uuid.uuid4().hex}"
    for attempt in range(12):
        try:
            path.mkdir()
            (path / "token").write_text(token)
            lock.token = token
            lock.acquired = True
            return
        except FileExistsError:
            try:
                age = time.time() - path.stat().st_mtime
            except OSError:
                age = 0
            if age > LOCK_TTL_SECONDS:
                shutil.rmtree(path, ignore_errors=True)
                continue
            time.sleep(0.25 * (attempt + 1))
    raise CacheError(f"Cache ocupado para {lock.abi}: {path}")


def release_lock(lock: cache_lock) -> None:
    if not lock.acquired:
        return
    try:
        token = (lock.path / "token").read_text()
        if token == lock.token:
            shutil.rmtree(lock.path, ignore_errors=True)
    except OSError:
        pass
    lock.acquired = False


def acquire_all(ctx: CacheContext, abis: Iterable[str] | None = None) -> list[cache_lock]:
    names = sorted(abis or ctx.abis)
    locks = [cache_lock(ctx, abi, ctx.lock_dir(abi), "") for abi in names]
    try:
        for lock in locks:
            acquire_lock(lock)
    except Exception:
        for lock in reversed(locks):
            release_lock(lock)
        raise
    return locks


def release_all(locks: Iterable[cache_lock]) -> None:
    for lock in reversed(list(locks)):
        release_lock(lock)


# ---------------------------------------------------------------------------
# Export/import de bundle
# ---------------------------------------------------------------------------


def _hmac_secret() -> bytes:
    return os.environ.get("APKFAST_CACHE_HMAC", "").encode("utf-8")


def _canonical_json(data: dict) -> str:
    return json.dumps(data, sort_keys=True, separators=(",", ":"))


def sign_manifest(manifest: dict) -> None:
    secret = _hmac_secret()
    if not secret:
        return
    payload = _canonical_json(manifest).encode("utf-8")
    manifest["signature"] = hmac.new(secret, payload, hashlib.sha256).hexdigest()


def verify_manifest(manifest: dict, *, required: bool) -> None:
    signature = manifest.get("signature")
    secret = _hmac_secret()
    if not signature:
        if required:
            raise CacheError("Bundle sem assinatura HMAC (--require-cache-signature)")
        return
    if not secret:
        raise CacheError("Bundle assinado, mas APKFAST_CACHE_HMAC não está definido")
    unsigned = dict(manifest)
    unsigned.pop("signature", None)
    expected = hmac.new(
        secret, _canonical_json(unsigned).encode("utf-8"), hashlib.sha256
    ).hexdigest()
    if not hmac.compare_digest(signature, expected):
        raise CacheError("Assinatura HMAC do bundle inválida")


def export_bundle(ctx: CacheContext, output: Path, abis: Iterable[str] | None = None) -> dict:
    output = output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    names = sorted(abis or ctx.abis)
    abi_data: dict[str, dict] = {}
    blobs: dict[str, dict] = {}

    for abi in names:
        cache = read_cache(ctx, abi)
        objects: dict[str, dict] = {}
        for key, entry in cache.get("objects", {}).items():
            if not isinstance(entry, dict):
                continue
            digest = entry.get("obj_blob")
            if not isinstance(digest, str) or not _BLOB_NAME.fullmatch(digest):
                continue
            path = ctx.blob_path(digest)
            if not path.is_file() or sha256_file(path) != digest:
                continue
            objects[key] = entry
            blobs[digest] = {"size": path.stat().st_size, "kind": "object"}
        link = cache.get("link")
        if isinstance(link, dict):
            digest = link.get("so_blob")
            if isinstance(digest, str) and _BLOB_NAME.fullmatch(digest):
                path = ctx.blob_path(digest)
                if path.is_file() and sha256_file(path) == digest:
                    blobs[digest] = {"size": path.stat().st_size, "kind": "shared"}
        extra: dict[str, dict] = {}
        for name in BUNDLE_ENTRIES:
            entry = cache.get(name)
            if not isinstance(entry, dict):
                continue
            digest = entry.get("blob")
            if isinstance(digest, str) and _BLOB_NAME.fullmatch(digest):
                path = ctx.blob_path(digest)
                if path.is_file() and sha256_file(path) == digest:
                    blobs[digest] = {"size": path.stat().st_size, "kind": name}
                    extra[name] = entry
        abi_data[abi] = {"objects": objects, "link": link, **extra}

    manifest = {
        "format": BUNDLE_FORMAT,
        "bundleVersion": BUNDLE_VERSION,
        "cacheSchema": CACHE_SCHEMA,
        "tool": "apkfast 2.3.0",
        "createdAt": datetime.now(timezone.utc).isoformat(),
        "projectId": ctx.project_id,
        "abis": abi_data,
        "blobs": blobs,
    }
    sign_manifest(manifest)

    tmp = output.with_suffix(output.suffix + f".tmp-{uuid.uuid4().hex}")
    try:
        with tarfile.open(tmp, "w:gz") as tar:
            raw = _canonical_json(manifest).encode("utf-8")
            info = tarfile.TarInfo("manifest.json")
            info.size = len(raw)
            tar.addfile(info, __import__("io").BytesIO(raw))
            for digest, meta in blobs.items():
                path = ctx.blob_path(digest)
                member = tarfile.TarInfo(f"blobs/{digest[:2]}/{digest}")
                member.size = meta["size"]
                with path.open("rb") as stream:
                    tar.addfile(member, stream)
        os.replace(tmp, output)
    except OSError as exc:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise CacheError(f"Falha ao gravar bundle {output}: {exc}") from exc
    return manifest


def _safe_members(tar: tarfile.TarFile) -> list[tarfile.TarInfo]:
    members: list[tarfile.TarInfo] = []
    for member in tar.getmembers():
        if not member.isfile():
            raise CacheError(f"Membro de bundle não é arquivo regular: {member.name}")
        name = member.name
        if name.startswith("/") or ".." in PurePosixPath(name).parts:
            raise CacheError(f"Nome de membro inseguro no bundle: {name}")
        if name == "manifest.json":
            valid = True
        else:
            parts = name.split("/")
            valid = (
                len(parts) == 3
                and parts[0] == "blobs"
                and bool(_BLOB_DIR.fullmatch(parts[1]))
                and bool(_BLOB_NAME.fullmatch(parts[2]))
                and parts[1] == parts[2][:2]
            )
        if not valid:
            raise CacheError(f"Membro de bundle inesperado: {name}")
        if member.size > MAX_BLOB_BYTES:
            raise CacheError(f"Membro de bundle grande demais: {name}")
        members.append(member)
    return members


def import_bundle(
    ctx: CacheContext,
    bundle: Path,
    *,
    abis: Iterable[str] | None = None,
    force: bool = False,
    require_signature: bool | None = None,
) -> dict:
    bundle = bundle.expanduser().resolve()
    wanted = set(abis or ctx.abis)
    required = ctx.require_signature if require_signature is None else require_signature
    stats = {"imported": 0, "skipped": 0, "rejected": 0}

    with tempfile.TemporaryDirectory(prefix="apkfast-cache-import-") as scratch:
        tmp_root = Path(scratch)
        try:
            with tarfile.open(bundle, "r:gz") as tar:
                members = _safe_members(tar)
                for member in members:
                    target = tmp_root / member.name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    source = tar.extractfile(member)
                    if source is None:
                        raise CacheError(f"Não foi possível ler {member.name}")
                    with source, target.open("wb") as out:
                        shutil.copyfileobj(source, out)
        except (OSError, tarfile.TarError) as exc:
            raise CacheError(f"Bundle inválido: {exc}") from exc

        manifest_path = tmp_root / "manifest.json"
        if not manifest_path.is_file():
            raise CacheError("Bundle sem manifest.json")
        try:
            manifest = json.loads(manifest_path.read_text())
        except (OSError, ValueError) as exc:
            raise CacheError(f"manifest.json inválido: {exc}") from exc
        if manifest.get("format") != BUNDLE_FORMAT:
            raise CacheError("Formato de bundle desconhecido")
        if manifest.get("bundleVersion") != BUNDLE_VERSION:
            raise CacheError("Versão de bundle incompatível")
        if manifest.get("cacheSchema") != CACHE_SCHEMA:
            raise CacheError("Schema de cache incompatível")
        if manifest.get("projectId") != ctx.project_id and not force:
            raise CacheError("projectId do bundle diverge do projeto atual (use --force)")
        verify_manifest(manifest, required=required)

        for digest, meta in manifest.get("blobs", {}).items():
            if not isinstance(digest, str) or not _BLOB_NAME.fullmatch(digest):
                stats["rejected"] += 1
                continue
            path = tmp_root / "blobs" / digest[:2] / digest
            if not path.is_file() or sha256_file(path) != digest:
                stats["rejected"] += 1
                continue
            if meta.get("size") not in (None, path.stat().st_size):
                stats["rejected"] += 1
                continue
            destination = ctx.blob_path(digest)
            if destination.is_file():
                stats["skipped"] += 1
                continue
            if not ctx.writable:
                stats["skipped"] += 1
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(path, destination)
            stats["imported"] += 1

        for abi, data in manifest.get("abis", {}).items():
            if abi not in wanted:
                continue
            entries = data.get("objects", {})
            for key, entry in entries.items():
                digest = entry.get("obj_blob") if isinstance(entry, dict) else None
                try:
                    ensure_blob(ctx, digest, abi=abi, check_elf=False)
                except CacheError:
                    stats["rejected"] += 1
            link = data.get("link")
            if isinstance(link, dict):
                try:
                    ensure_blob(ctx, link.get("so_blob"), abi=abi)
                except CacheError:
                    stats["rejected"] += 1
            extra: dict[str, dict] = {}
            for name in BUNDLE_ENTRIES:
                entry = data.get(name)
                if not isinstance(entry, dict):
                    continue
                try:
                    # DEX não é ELF; resource/APK/AAB são ZIP, mas o SHA256
                    # já basta aqui (validado na leitura por resource_cache).
                    ensure_blob(ctx, entry.get("blob"), abi=abi, check_elf=False)
                except CacheError:
                    stats["rejected"] += 1
                    continue
                extra[name] = entry
            if not ctx.writable:
                continue
            target = ctx.cache_file(abi)
            current = read_cache(ctx, abi)
            current.setdefault("objects", {}).update(entries)
            if isinstance(link, dict):
                current["link"] = link
            current.update(extra)
            write_cache(ctx, abi, current)
    return stats
