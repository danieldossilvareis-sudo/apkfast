"""Build/install da raylib para Android via NDK + CMake.

A fonte é fixada por versão + SHA256 do tarball de release (ver constantes
abaixo). Overrides controlados: ``APKFAST_RAYLIB_VERSION`` e
``APKFAST_RAYLIB_SHA256`` (este último obrigatório quando a versão muda).
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

from .toolchain import ABI_TRIPLES, detect

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "vendor" / "raylib"
SRC_CACHE = Path(
    os.environ.get("APKFAST_RAYLIB_SRC", str(VENDOR / "src"))
)

# Pino da fonte: release estável da série 5 (API usada pelo scaffold v2.5.0).
# Tag 5.5 -> commit c1ab645ca298a2801097931d1079b10ff7eb9df8.
RAYLIB_VERSION = "5.5"
RAYLIB_COMMIT = "c1ab645ca298a2801097931d1079b10ff7eb9df8"
RAYLIB_SHA256 = "aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e"
RAYLIB_URL = (
    "https://github.com/raysan5/raylib/archive/refs/tags/{version}.tar.gz"
)
ENV_VERSION = "APKFAST_RAYLIB_VERSION"
ENV_SHA256 = "APKFAST_RAYLIB_SHA256"
PIN_FILE = ".apkfast-source.json"

_VERSION_RE = re.compile(r"^\d+\.\d+(?:\.\d+)?$")
_SHA_RE = re.compile(r"^[0-9a-fA-F]{64}$")


class RaylibError(RuntimeError):
    pass


def _say(message: str, quiet: bool = False) -> None:
    if not quiet:
        print(message, flush=True)


def _run(cmd: list[str], *, quiet: bool = False) -> None:
    _say(f"  $ {' '.join(str(c) for c in cmd)}", quiet)
    try:
        result = subprocess.run([str(c) for c in cmd], check=False)
    except OSError as exc:
        raise RaylibError(f"Não foi possível executar {cmd[0]}: {exc}") from exc
    if result.returncode != 0:
        raise RaylibError(f"Comando falhou: {' '.join(str(c) for c in cmd)}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _resolve_pin(env: dict[str, str] | None = None) -> tuple[str, str, str]:
    """Versão, SHA256 e URL efetivos, com overrides validados."""

    environ = os.environ if env is None else env
    version = (environ.get(ENV_VERSION) or RAYLIB_VERSION).strip()
    if not _VERSION_RE.fullmatch(version):
        raise RaylibError(
            f"Versão da raylib inválida: {version!r}. Use X.Y (ex.: {RAYLIB_VERSION})."
        )
    raw_sha = (environ.get(ENV_SHA256) or "").strip()
    if version == RAYLIB_VERSION and not raw_sha:
        sha256 = RAYLIB_SHA256
    elif raw_sha:
        if not _SHA_RE.fullmatch(raw_sha):
            raise RaylibError(f"{ENV_SHA256} deve ter 64 dígitos hexadecimais.")
        sha256 = raw_sha.lower()
    else:
        raise RaylibError(
            f"Versão alternativa da raylib ({version}) exige {ENV_SHA256} "
            "com o SHA256 do tarball. Sem digest não há reprodutibilidade."
        )
    return version, sha256, RAYLIB_URL.format(version=version)


def _download(url: str, archive: Path, expected: str, version: str, *, quiet: bool) -> None:
    tmp = archive.with_name(archive.name + ".part")
    _say(f"[*] Baixando raylib {version} ...", quiet)
    try:
        request = urllib.request.Request(
            url, headers={"User-Agent": "apkfast-raylib-pinned"}
        )
        with urllib.request.urlopen(request, timeout=60) as response, tmp.open("wb") as out:
            shutil.copyfileobj(response, out)
    except (OSError, urllib.error.URLError) as exc:
        tmp.unlink(missing_ok=True)
        raise RaylibError(f"Falha ao baixar tarball da raylib {version}: {exc}") from exc
    try:
        actual = _sha256(tmp)
        if actual != expected:
            raise RaylibError(
                f"SHA256 da raylib {version} divergiu: obtido {actual}, esperado {expected}. "
                f"Verifique {ENV_VERSION}/{ENV_SHA256} e a integridade do download."
            )
        tmp.replace(archive)
    finally:
        tmp.unlink(missing_ok=True)


def _check_pin_marker(source: Path, version: str, sha256: str) -> None:
    marker = source / PIN_FILE
    if not marker.is_file():
        # Fonte pré-existente (ex.: checkout de desenvolvimento) é respeitada.
        return
    try:
        doc = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise RaylibError(f"Marcação de versão da raylib inválida em {marker}: {exc}") from exc
    old = f"{doc.get('version')}:{doc.get('sha256')}"
    if old != f"{version}:{sha256}":
        raise RaylibError(
            f"A fonte em {source} corresponde a {old}, não a raylib {version} "
            f"({sha256}). Remova o diretório ou ajuste {ENV_VERSION}/{ENV_SHA256}."
        )


def _safe_link(member_name: str, link_name: str, top: str) -> bool:
    """Aceita links relativos que permaneçam dentro do diretório extraído."""

    if not link_name or link_name.startswith("/"):
        return False
    parts: list[str] = []
    for part in (Path(member_name).parent / link_name).parts:
        if part == "..":
            if not parts:
                return False
            parts.pop()
        elif part not in ("", "."):
            parts.append(part)
    return bool(parts) and parts[0] == top


def _has_sources(path: Path) -> bool:
    return (path / "src" / "raylib.h").is_file() or (path / "raylib.h").is_file()


def ensure_source(quiet: bool = False) -> Path:
    version, sha256, url = _resolve_pin()
    if _has_sources(SRC_CACHE):
        _check_pin_marker(SRC_CACHE, version, sha256)
        return SRC_CACHE
    if SRC_CACHE.exists():
        if not SRC_CACHE.is_dir():
            raise RaylibError(f"APKFAST_RAYLIB_SRC não é um diretório: {SRC_CACHE}")
        if any(SRC_CACHE.iterdir()):
            raise RaylibError(
                f"Fonte da raylib incompleta em {SRC_CACHE}. "
                "Remova o diretório ou ajuste APKFAST_RAYLIB_SRC."
            )

    SRC_CACHE.parent.mkdir(parents=True, exist_ok=True)
    archive = SRC_CACHE.parent / f"raylib-{version}.tar.gz"
    if not archive.is_file() or _sha256(archive) != sha256:
        _download(url, archive, sha256, version, quiet=quiet)

    top = f"raylib-{version}"
    staging = Path(tempfile.mkdtemp(prefix=".raylib-src-", dir=SRC_CACHE.parent))
    try:
        with tarfile.open(archive, "r:gz") as tar:
            for member in tar.getmembers():
                name = member.name
                if name.startswith("/") or ".." in Path(name).parts:
                    raise RaylibError(f"Membro inseguro no tarball da raylib: {name}")
                if member.issym() or member.islnk():
                    if not _safe_link(name, member.linkname, top):
                        raise RaylibError(
                            "Link inseguro no tarball da raylib: "
                            f"{name} -> {member.linkname}"
                        )
                elif not (member.isfile() or member.isdir()):
                    raise RaylibError(f"Membro inseguro no tarball da raylib: {name}")
            tar.extractall(staging)
        extracted = staging / top
        if not _has_sources(extracted):
            raise RaylibError(f"Tarball da raylib {version} não contém src/raylib.h")
        if SRC_CACHE.exists():
            SRC_CACHE.rmdir()
        shutil.move(str(extracted), str(SRC_CACHE))
        (SRC_CACHE / PIN_FILE).write_text(
            json.dumps(
                {"version": version, "sha256": sha256, "url": url},
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    if not _has_sources(SRC_CACHE):
        raise RaylibError(f"Tarball da raylib não contém raylib.h em {SRC_CACHE}")
    return SRC_CACHE


def install(abi: str = "arm64-v8a", api: int = 36, *, quiet: bool = False) -> Path:
    if abi not in ABI_TRIPLES:
        raise RaylibError(f"ABI não suportada: {abi}")
    effective_api = max(int(api), 21)
    tc = detect(abi=abi, api=effective_api)

    source = ensure_source(quiet)
    cmake = shutil.which("cmake")
    if not cmake:
        raise RaylibError("cmake não encontrado (necessário para compilar a raylib)")

    ndk_toolchain = tc.ndk_dir / "build" / "cmake" / "android.toolchain.cmake"
    if not ndk_toolchain.is_file():
        raise RaylibError(f"Android toolchain do CMake não encontrado: {ndk_toolchain}")

    build_dir = source.parent / f"raylib-build-{abi}"
    out_lib = VENDOR / "lib" / f"libraylib-{abi}.a"
    out_inc = VENDOR / "include"

    src_dir = source / "src" if (source / "src" / "raylib.h").is_file() else source
    if not (src_dir / "raylib.h").is_file():
        raise RaylibError(f"raylib.h não encontrado em {src_dir}")

    _say(f"[*] Compilando raylib para {abi} (API {effective_api}) ...", quiet)
    _run([
        cmake, "-S", str(source), "-B", str(build_dir),
        f"-DCMAKE_TOOLCHAIN_FILE={ndk_toolchain}",
        f"-DANDROID_ABI={abi}",
        f"-DANDROID_PLATFORM=android-{effective_api}",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DPLATFORM=Android",
        "-DBUILD_EXAMPLES=OFF",
    ], quiet=quiet)
    # Compila apenas o target raylib (evita exemplos que exigem GL desktop).
    _run([cmake, "--build", str(build_dir), "--target", "raylib", "-j4"], quiet=quiet)

    built = next(
        (path for path in (
            build_dir / "raylib" / "libraylib.a",
            build_dir / "libraylib.a",
            build_dir / "src" / "libraylib.a",
        ) if path.is_file()),
        None,
    )
    if built is None:
        raise RaylibError(f"libraylib.a não foi gerada em {build_dir}")

    out_lib.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, out_lib)
    out_inc.mkdir(parents=True, exist_ok=True)
    for header in ("raylib.h", "rlgl.h", "raymath.h", "rcamera.h"):
        candidate = src_dir / header
        if candidate.is_file():
            shutil.copy2(candidate, out_inc / header)

    _say(f"[✓] raylib instalada: {out_lib} ({out_lib.stat().st_size // 1024} KiB)", quiet)
    return out_lib
