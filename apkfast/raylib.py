"""Build/install da raylib para Android via NDK + CMake."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

from .toolchain import ABI_TRIPLES, detect

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "vendor" / "raylib"
SRC_CACHE = Path(
    os.environ.get("APKFAST_RAYLIB_SRC", str(VENDOR / "src"))
)


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


def _has_sources(path: Path) -> bool:
    return (path / "src" / "raylib.h").is_file() or (path / "raylib.h").is_file()


def ensure_source(quiet: bool = False) -> Path:
    if _has_sources(SRC_CACHE):
        return SRC_CACHE
    if SRC_CACHE.exists():
        if not SRC_CACHE.is_dir():
            raise RaylibError(f"APKFAST_RAYLIB_SRC não é um diretório: {SRC_CACHE}")
        if any(SRC_CACHE.iterdir()):
            raise RaylibError(
                f"Fonte da raylib incompleta em {SRC_CACHE}. "
                "Remova o diretório ou ajuste APKFAST_RAYLIB_SRC."
            )

    git = shutil.which("git")
    if not git:
        raise RaylibError("git não encontrado (necessário para baixar a raylib)")

    SRC_CACHE.parent.mkdir(parents=True, exist_ok=True)
    _say(f"[*] Baixando raylib em {SRC_CACHE} ...", quiet)
    created = not SRC_CACHE.exists()
    try:
        _run(
            [
                git, "clone", "--depth", "1",
                "https://github.com/raysan5/raylib.git", str(SRC_CACHE),
            ],
            quiet=quiet,
        )
    except RaylibError:
        if created:
            shutil.rmtree(SRC_CACHE, ignore_errors=True)
        raise
    if not _has_sources(SRC_CACHE):
        raise RaylibError(f"Clone da raylib não contém raylib.h em {SRC_CACHE}")
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
