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


def _run(cmd: list[str], *, quiet: bool = False) -> None:
    if not quiet:
        print(f"  $ {' '.join(str(c) for c in cmd)}", flush=True)
    result = subprocess.run([str(c) for c in cmd])
    if result.returncode != 0:
        raise RaylibError(f"Comando falhou: {' '.join(str(c) for c in cmd)}")


def ensure_source(quiet: bool = False) -> Path:
    if (SRC_CACHE / "src" / "raylib.h").is_file() or (SRC_CACHE / "raylib.h").is_file():
        return SRC_CACHE
    SRC_CACHE.parent.mkdir(parents=True, exist_ok=True)
    print(f"[*] Baixando raylib em {SRC_CACHE} ...")
    _run(
        ["git", "clone", "--depth", "1", "https://github.com/raysan5/raylib.git", str(SRC_CACHE)],
        quiet=quiet,
    )
    return SRC_CACHE


def install(abi: str = "arm64-v8a", api: int = 36, *, quiet: bool = False) -> Path:
    if abi not in ABI_TRIPLES:
        raise RaylibError(f"ABI não suportada: {abi}")
    tc = detect(abi=abi, api=api)

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

    print(f"[*] Compilando raylib para {abi} (API {api}) ...")
    _run([
        cmake, "-S", str(source), "-B", str(build_dir),
        f"-DCMAKE_TOOLCHAIN_FILE={ndk_toolchain}",
        f"-DANDROID_ABI={abi}",
        f"-DANDROID_PLATFORM=android-{api}",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DPLATFORM=Android",
        "-DBUILD_EXAMPLES=OFF",
    ], quiet=quiet)
    # Compila apenas o target raylib (evita exemplos que exigem GL desktop).
    _run([cmake, "--build", str(build_dir), "--target", "raylib", "-j4"], quiet=quiet)

    built = build_dir / "raylib" / "libraylib.a"
    if not built.is_file():
        built = build_dir / "libraylib.a"
    if not built.is_file():
        raise RaylibError(f"libraylib.a não foi gerada em {build_dir}")

    out_lib.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built, out_lib)
    out_inc.mkdir(parents=True, exist_ok=True)
    for header in ("raylib.h", "rlgl.h", "raymath.h", "rcamera.h"):
        candidate = src_dir / header
        if candidate.is_file():
            shutil.copy2(candidate, out_inc / header)

    print(f"[✓] raylib instalada: {out_lib} ({out_lib.stat().st_size // 1024} KiB)")
    return out_lib
