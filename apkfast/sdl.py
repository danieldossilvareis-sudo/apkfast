"""Build/install do SDL2 e SDL3 para Android via NDK + CMake."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

from .toolchain import ABI_TRIPLES, detect

ROOT = Path(__file__).resolve().parent.parent
REPOS = {
    2: "https://github.com/libsdl-org/SDL.git",
    3: "https://github.com/libsdl-org/SDL.git",
}
BRANCHES = {2: "release-2.32.6", 3: None}


class SDLError(RuntimeError):
    pass


def _run(cmd: list[str], *, quiet: bool = False) -> None:
    if not quiet:
        print(f"  $ {' '.join(str(c) for c in cmd)}", flush=True)
    result = subprocess.run([str(c) for c in cmd])
    if result.returncode != 0:
        raise SDLError(f"Comando falhou: {' '.join(str(c) for c in cmd)}")


def ensure_source(version: int, *, quiet: bool = False) -> Path:
    vendor = ROOT / "vendor" / f"sdl{version}"
    source = vendor / "src"
    if (source / "include" / "SDL.h").is_file() or (
        source / "include" / "SDL3" / "SDL.h"
    ).is_file():
        return source

    source.parent.mkdir(parents=True, exist_ok=True)
    print(f"[*] Baixando SDL{version} em {source} ...")
    cmd = ["git", "clone", "--depth", "1"]
    if BRANCHES.get(version):
        cmd += ["--branch", BRANCHES[version]]
    cmd += [REPOS[version], str(source)]
    _run(cmd, quiet=quiet)
    return source


def install(version: int, abi: str = "arm64-v8a", api: int = 21, *, quiet: bool = False) -> Path:
    if version not in (2, 3):
        raise SDLError(f"Versão SDL inválida: {version}")
    if abi not in ABI_TRIPLES:
        raise SDLError(f"ABI não suportada: {abi}")
    effective_api = max(api, 21)
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

    print(f"[*] Compilando SDL{version} para {abi} ...")
    _run([
        cmake, "-S", str(source), "-B", str(build_dir),
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DANDROID_ABI={abi}",
        f"-DANDROID_PLATFORM=android-{effective_api}",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DSDL_TEST=OFF",
        "-DSDL_EXAMPLES=OFF",
    ], quiet=quiet)
    _run([cmake, "--build", str(build_dir), "--target", f"SDL{version}-static", "-j4"],
         quiet=quiet)

    built = build_dir / f"libSDL{version}.a"
    if not built.is_file():
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

    print(f"[✓] SDL{version} instalada: {out_lib} ({out_lib.stat().st_size // 1024} KiB)")
    return out_lib
