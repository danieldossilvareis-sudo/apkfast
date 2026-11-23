"""Build/install do SDL2 e SDL3 para Android via NDK + CMake.

Cada série é fixada por versão + SHA256 do tarball oficial de release.
Overrides controlados: ``APKFAST_SDL2_VERSION``/``APKFAST_SDL3_VERSION`` e
``APKFAST_SDL2_SHA256``/``APKFAST_SDL3_SHA256`` (o digest é obrigatório quando
a versão é alterada).
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

# Pinos oficiais (assets de release com digest publicado no GitHub):
#   SDL2 2.32.10 -> tag release-2.32.10, commit 5d249570393f7a37e037abf22cd6012a4cc56a71
#   SDL3  3.4.20 -> tag release-3.4.20, commit f00dc919ca35b9756316e86c4777392de442f9b2
SDL2_VERSION = "2.32.10"
SDL2_SHA256 = "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"
SDL2_COMMIT = "5d249570393f7a37e037abf22cd6012a4cc56a71"
SDL3_VERSION = "3.4.20"
SDL3_SHA256 = "f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f"
SDL3_COMMIT = "f00dc919ca35b9756316e86c4777392de442f9b2"

ENV_VERSION = {2: "APKFAST_SDL2_VERSION", 3: "APKFAST_SDL3_VERSION"}
ENV_SHA256 = {2: "APKFAST_SDL2_SHA256", 3: "APKFAST_SDL3_SHA256"}
PIN_FILE = ".apkfast-source.json"

_URL_TEMPLATE = (
    "https://github.com/libsdl-org/SDL/releases/download/"
    "release-{version}/SDL{major}-{version}.tar.gz"
)
_VERSION_RE = re.compile(r"^\d+\.\d+(?:\.\d+)?$")
_SHA_RE = re.compile(r"^[0-9a-fA-F]{64}$")


class SDLError(RuntimeError):
    pass


def _say(message: str, quiet: bool = False) -> None:
    if not quiet:
        print(message, flush=True)


def _run(cmd: list[str], *, quiet: bool = False) -> None:
    _say(f"  $ {' '.join(str(c) for c in cmd)}", quiet)
    try:
        result = subprocess.run([str(c) for c in cmd], check=False)
    except OSError as exc:
        raise SDLError(f"Não foi possível executar {cmd[0]}: {exc}") from exc
    if result.returncode != 0:
        raise SDLError(f"Comando falhou: {' '.join(str(c) for c in cmd)}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _resolve_pin(major: int, env: dict[str, str] | None = None) -> tuple[str, str, str]:
    """Versão, SHA256 e URL efetivos para SDL2/SDL3."""

    if major not in (2, 3):
        raise SDLError(f"Versão SDL inválida: {major}")
    environ = os.environ if env is None else env
    pinned_version = SDL2_VERSION if major == 2 else SDL3_VERSION
    pinned_sha = SDL2_SHA256 if major == 2 else SDL3_SHA256
    version = (environ.get(ENV_VERSION[major]) or pinned_version).strip()
    if not _VERSION_RE.fullmatch(version):
        raise SDLError(
            f"Versão SDL{major} inválida: {version!r}. "
            f"Use X.Y.Z (ex.: {pinned_version})."
        )
    raw_sha = (environ.get(ENV_SHA256[major]) or "").strip()
    if version == pinned_version and not raw_sha:
        sha256 = pinned_sha
    elif raw_sha:
        if not _SHA_RE.fullmatch(raw_sha):
            raise SDLError(f"{ENV_SHA256[major]} deve ter 64 dígitos hexadecimais.")
        sha256 = raw_sha.lower()
    else:
        raise SDLError(
            f"Versão alternativa do SDL{major} ({version}) exige "
            f"{ENV_SHA256[major]} com o SHA256 do tarball. "
            "Sem digest não há reprodutibilidade."
        )
    return version, sha256, _URL_TEMPLATE.format(major=major, version=version)


def _download(
    url: str, archive: Path, expected: str, version: str, major: int, *, quiet: bool
) -> None:
    tmp = archive.with_name(archive.name + ".part")
    _say(f"[*] Baixando SDL{major} {version} ...", quiet)
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "apkfast-sdl-pinned"})
        with urllib.request.urlopen(request, timeout=60) as response, tmp.open("wb") as out:
            shutil.copyfileobj(response, out)
    except (OSError, urllib.error.URLError) as exc:
        tmp.unlink(missing_ok=True)
        raise SDLError(f"Falha ao baixar tarball do SDL{major} {version}: {exc}") from exc
    try:
        actual = _sha256(tmp)
        if actual != expected:
            raise SDLError(
                f"SHA256 do SDL{major} {version} divergiu: obtido {actual}, "
                f"esperado {expected}. Verifique {ENV_VERSION[major]}/"
                f"{ENV_SHA256[major]} e a integridade do download."
            )
        tmp.replace(archive)
    finally:
        tmp.unlink(missing_ok=True)


def _check_pin_marker(source: Path, version: str, sha256: str, major: int) -> None:
    marker = source / PIN_FILE
    if not marker.is_file():
        # Fonte pré-existente (ex.: checkout de desenvolvimento) é respeitada.
        return
    try:
        doc = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise SDLError(f"Marcação de versão do SDL{major} inválida em {marker}: {exc}") from exc
    old = f"{doc.get('version')}:{doc.get('sha256')}"
    if old != f"{version}:{sha256}":
        raise SDLError(
            f"A fonte em {source} corresponde a {old}, não a SDL{major} {version} "
            f"({sha256}). Remova o diretório ou ajuste "
            f"{ENV_VERSION[major]}/{ENV_SHA256[major]}."
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


def _has_sources(source: Path, version: int) -> bool:
    candidates = (
        (source / "include" / "SDL.h",)
        if version == 2
        else (source / "include" / "SDL3" / "SDL.h",)
    )
    return any(candidate.is_file() for candidate in candidates)


def ensure_source(version: int, *, quiet: bool = False) -> Path:
    if version not in (2, 3):
        raise SDLError(f"Versão SDL inválida: {version}")
    pin_version, sha256, url = _resolve_pin(version)
    vendor = ROOT / "vendor" / f"sdl{version}"
    source = vendor / "src"
    if _has_sources(source, version):
        _check_pin_marker(source, pin_version, sha256, version)
        return source
    if source.exists():
        if not source.is_dir():
            raise SDLError(f"Caminho de fonte SDL{version} não é diretório: {source}")
        if any(source.iterdir()):
            raise SDLError(
                f"Fonte SDL{version} incompleta em {source}. "
                "Remova o diretório e rode setup-libs novamente."
            )

    source.parent.mkdir(parents=True, exist_ok=True)
    archive = source.parent / f"SDL{version}-{pin_version}.tar.gz"
    if not archive.is_file() or _sha256(archive) != sha256:
        _download(url, archive, sha256, pin_version, version, quiet=quiet)

    top = f"SDL{version}-{pin_version}"
    staging = Path(tempfile.mkdtemp(prefix=f".sdl{version}-src-", dir=source.parent))
    try:
        with tarfile.open(archive, "r:gz") as tar:
            for member in tar.getmembers():
                name = member.name
                if name.startswith("/") or ".." in Path(name).parts:
                    raise SDLError(f"Membro inseguro no tarball do SDL{version}: {name}")
                if member.issym() or member.islnk():
                    if not _safe_link(name, member.linkname, top):
                        raise SDLError(
                            f"Link inseguro no tarball do SDL{version}: "
                            f"{name} -> {member.linkname}"
                        )
                elif not (member.isfile() or member.isdir()):
                    raise SDLError(f"Membro inseguro no tarball do SDL{version}: {name}")
            tar.extractall(staging)
        extracted = staging / top
        if not _has_sources(extracted, version):
            raise SDLError(
                f"Tarball do SDL{version} {pin_version} não contém os headers esperados"
            )
        if source.exists():
            source.rmdir()
        shutil.move(str(extracted), str(source))
        (source / PIN_FILE).write_text(
            json.dumps(
                {"version": pin_version, "sha256": sha256, "url": url},
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    if not _has_sources(source, version):
        raise SDLError(f"Fonte SDL{version} inválida em {source}")
    return source


def install(
    version: int, abi: str = "arm64-v8a", api: int = 21, *, quiet: bool = False
) -> Path:
    if version not in (2, 3):
        raise SDLError(f"Versão SDL inválida: {version}")
    if abi not in ABI_TRIPLES:
        raise SDLError(f"ABI não suportada: {abi}")
    effective_api = max(int(api), 21)
    tc = detect(abi=abi, api=effective_api)

    source = ensure_source(version, quiet=quiet)
    cmake = shutil.which("cmake")
    if not cmake:
        raise SDLError("cmake não encontrado")
    toolchain = tc.ndk_dir / "build" / "cmake" / "android.toolchain.cmake"
    if not toolchain.is_file():
        raise SDLError(f"Toolchain CMake não encontrado: {toolchain}")

    build_dir = source.parent / f"build-{abi}"
    out_lib = source.parent / "lib" / f"libSDL{version}-{abi}.a"
    out_inc = source.parent / "include"

    # Opções reais de cada versão: SDL2 usa SDL_TEST; SDL3 usa SDL_TEST_LIBRARY.
    # SDL_STATIC_PIC só existe no SDL2 e é necessário para linkar dentro de
    # libmain.so; em SDL3 o toolchain Android já gera código PIC.
    cmake_options = ["-DSDL_SHARED=OFF", "-DSDL_STATIC=ON", "-DSDL_TESTS=OFF"]
    if version == 2:
        cmake_options += ["-DSDL_TEST=OFF", "-DSDL_STATIC_PIC=ON"]
    else:
        cmake_options += ["-DSDL_TEST_LIBRARY=OFF", "-DSDL_EXAMPLES=OFF"]

    _say(f"[*] Compilando SDL{version} para {abi} (API {effective_api}) ...", quiet)
    _run([
        cmake, "-S", str(source), "-B", str(build_dir),
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DANDROID_ABI={abi}",
        f"-DANDROID_PLATFORM=android-{effective_api}",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        *cmake_options,
    ], quiet=quiet)
    _run(
        [cmake, "--build", str(build_dir), "--target", f"SDL{version}-static", "-j4"],
        quiet=quiet,
    )

    built = next(
        (path for path in (
            build_dir / f"libSDL{version}.a",
            build_dir / "lib" / f"libSDL{version}.a",
        ) if path.is_file()),
        None,
    )
    if built is None:
        raise SDLError(f"libSDL{version}.a não foi gerada em {build_dir}")

    out_lib.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, out_lib)
    out_inc.mkdir(parents=True, exist_ok=True)

    if version == 2:
        for header in (source / "include").glob("*.h"):
            shutil.copy2(header, out_inc / header.name)
    else:
        dest = out_inc / "SDL3"
        dest.mkdir(parents=True, exist_ok=True)
        for header in (source / "include" / "SDL3").glob("*.h"):
            shutil.copy2(header, dest / header.name)

    _say(f"[✓] SDL{version} instalada: {out_lib} ({out_lib.stat().st_size // 1024} KiB)", quiet)
    return out_lib
