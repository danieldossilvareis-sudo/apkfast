"""Build/install da libwebp (decoder + demux) para Android via NDK + CMake."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path
from urllib.request import urlretrieve

from .toolchain import ABI_TRIPLES, detect

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "vendor" / "webp"
SRC_CACHE = Path(os.environ.get("APKFAST_WEBP_SRC", str(VENDOR / "src")))

VERSION = "1.6.0"
TARBALL = f"libwebp-{VERSION}.tar.gz"
URL = (
    "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/"
    + TARBALL
)
SHA256 = "e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"
HEADERS = ("types.h", "decode.h", "demux.h", "mux_types.h")
LICENSES = ("COPYING", "PATENTS", "AUTHORS")


class WebPError(RuntimeError):
    pass


def _say(message: str, quiet: bool = False) -> None:
    if not quiet:
        print(message, flush=True)


def _run(cmd: list[str], *, quiet: bool = False) -> None:
    _say(f"  $ {' '.join(str(c) for c in cmd)}", quiet)
    try:
        result = subprocess.run([str(c) for c in cmd], check=False)
    except OSError as exc:
        raise WebPError(f"Não foi possível executar {cmd[0]}: {exc}") from exc
    if result.returncode != 0:
        raise WebPError(f"Comando falhou: {' '.join(str(c) for c in cmd)}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _has_sources(path: Path) -> bool:
    return (path / "src" / "webp" / "demux.h").is_file()


def ensure_source(*, quiet: bool = False) -> Path:
    if _has_sources(SRC_CACHE):
        return SRC_CACHE
    SRC_CACHE.mkdir(parents=True, exist_ok=True)
    if any(SRC_CACHE.iterdir()):
        raise WebPError(
            f"Fonte libwebp incompleta em {SRC_CACHE}. Remova o diretório "
            "ou ajuste APKFAST_WEBP_SRC."
        )

    archive = SRC_CACHE.parent / TARBALL
    if not archive.is_file() or _sha256(archive) != SHA256:
        _say(f"[*] Baixando libwebp {VERSION} ...", quiet)
        with tempfile.NamedTemporaryFile(
            prefix="libwebp-", suffix=".tar.gz", delete=False
        ) as handle:
            tmp = Path(handle.name)
        try:
            urlretrieve(URL, tmp)
            actual = _sha256(tmp)
            if actual != SHA256:
                raise WebPError(
                    f"SHA256 da libwebp inválido: {actual} (esperado {SHA256})"
                )
            shutil.move(str(tmp), archive)
        finally:
            tmp.unlink(missing_ok=True)

    _say(f"[*] Extraindo {archive.name} ...", quiet)
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            name = member.name
            if name.startswith("/") or ".." in Path(name).parts:
                raise WebPError(f"Membro inseguro no tarball libwebp: {name}")
        tar.extractall(SRC_CACHE.parent)
    extracted = SRC_CACHE.parent / f"libwebp-{VERSION}"
    if not _has_sources(extracted):
        raise WebPError(f"Fonte libwebp inválida em {extracted}")
    if extracted != SRC_CACHE:
        extracted.rename(SRC_CACHE)
    return SRC_CACHE


def install(abi: str = "arm64-v8a", api: int = 21, *, quiet: bool = False) -> Path:
    if abi not in ABI_TRIPLES:
        raise WebPError(f"ABI não suportada: {abi}")
    effective_api = max(int(api), 21)
    tc = detect(abi=abi, api=effective_api)

    source = ensure_source(quiet=quiet)
    cmake = shutil.which("cmake")
    if not cmake:
        raise WebPError("cmake não encontrado (necessário para compilar a libwebp)")
    toolchain = tc.ndk_dir / "build" / "cmake" / "android.toolchain.cmake"
    if not toolchain.is_file():
        raise WebPError(f"Android toolchain do CMake não encontrado: {toolchain}")

    build_dir = VENDOR / f"build-{abi}"
    decoder = build_dir / "libwebpdecoder.a"
    demux = build_dir / "libwebpdemux.a"

    args = [
        cmake, "-S", str(source), "-B", str(build_dir),
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DANDROID_ABI={abi}",
        f"-DANDROID_PLATFORM=android-{effective_api}",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DWEBP_LINK_STATIC=ON",
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON",
        "-DWEBP_ENABLE_SIMD=OFF",
        "-DWEBP_USE_THREAD=OFF",
        "-DWEBP_BUILD_ANIM_UTILS=OFF",
        "-DWEBP_BUILD_CWEBP=OFF",
        "-DWEBP_BUILD_DWEBP=OFF",
        "-DWEBP_BUILD_GIF2WEBP=OFF",
        "-DWEBP_BUILD_IMG2WEBP=OFF",
        "-DWEBP_BUILD_VWEBP=OFF",
        "-DWEBP_BUILD_WEBPINFO=OFF",
        "-DWEBP_BUILD_LIBWEBPMUX=OFF",
        "-DWEBP_BUILD_WEBPMUX=OFF",
        "-DWEBP_BUILD_EXTRAS=OFF",
        "-DWEBP_BUILD_WEBP_JS=OFF",
        "-DWEBP_BUILD_FUZZTEST=OFF",
    ]
    _say(f"[*] Compilando libwebp {VERSION} para {abi} (API {effective_api}) ...", quiet)
    _run(args, quiet=quiet)
    _run(
        [cmake, "--build", str(build_dir), "--target", "webpdecoder", "webpdemux", "-j4"],
        quiet=quiet,
    )
    if not decoder.is_file() or not demux.is_file():
        raise WebPError(f"libwebpdecoder.a/libwebpdemux.a não geradas em {build_dir}")

    lib_dir = VENDOR / "lib"
    include_dir = VENDOR / "include" / "webp"
    lib_dir.mkdir(parents=True, exist_ok=True)
    include_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(decoder, lib_dir / f"libwebpdecoder-{abi}.a")
    shutil.copy2(demux, lib_dir / f"libwebpdemux-{abi}.a")
    for header in HEADERS:
        candidate = source / "src" / "webp" / header
        if candidate.is_file():
            shutil.copy2(candidate, include_dir / header)
    for license_name in LICENSES:
        candidate = source / license_name
        if candidate.is_file():
            shutil.copy2(candidate, include_dir / license_name)

    out = lib_dir / f"libwebpdemux-{abi}.a"
    _say(f"[✓] libwebp instalada: {out} ({out.stat().st_size // 1024} KiB)", quiet)
    return out
