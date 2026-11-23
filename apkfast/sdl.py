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
    vendor = ROOT / "vendor" / f"sdl{version}"
    source = vendor / "src"
    if _has_sources(source, version):
        return source
    if source.exists():
        if not source.is_dir():
            raise SDLError(f"Caminho de fonte SDL{version} não é diretório: {source}")
        if any(source.iterdir()):
            raise SDLError(
                f"Fonte SDL{version} incompleta em {source}. "
                "Remova o diretório e rode setup-libs novamente."
            )

    git = shutil.which("git")
    if not git:
        raise SDLError("git não encontrado (necessário para baixar a SDL)")

    source.parent.mkdir(parents=True, exist_ok=True)
    _say(f"[*] Baixando SDL{version} em {source} ...", quiet)
    cmd = [git, "clone", "--depth", "1"]
    if BRANCHES.get(version):
        cmd += ["--branch", BRANCHES[version]]
    cmd += [REPOS[version], str(source)]
    created = not source.exists()
    try:
        _run(cmd, quiet=quiet)
    except SDLError:
        if created:
            shutil.rmtree(source, ignore_errors=True)
        raise
    if not _has_sources(source, version):
        raise SDLError(f"Clone da SDL{version} não contém SDL.h em {source}")
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
