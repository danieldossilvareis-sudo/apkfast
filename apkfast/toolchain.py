"""Discovery of Android SDK/NDK tools, with host-architecture awareness.

On Termux/arm64 some SDK build-tools directories contain x86_64 binaries that
cannot be executed. Every candidate is probed before being selected.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import re
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterable

HOME = Path(os.environ.get("HOME", str(Path.home())))
ANDROID_ROOT = HOME / "Android"

ABI_TRIPLES: dict[str, str] = {
    "arm64-v8a": "aarch64-linux-android",
    "armeabi-v7a": "armv7a-linux-androideabi",
    "x86": "i686-linux-android",
    "x86_64": "x86_64-linux-android",
}

# Mesmo valor de ABI_TRIPLES: manter um único dict evita divergência futura.
ABI_CLANG_PREFIX: dict[str, str] = ABI_TRIPLES

NDK_ARCH_DIR: dict[str, str] = {
    "arm64-v8a": "arm64-v8a",
    "armeabi-v7a": "armeabi-v7a",
    "x86": "x86",
    "x86_64": "x86_64",
}


def _executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def _runnable(path: Path) -> bool:
    """Check that a candidate can actually run on this host."""
    if not _executable(path):
        return False
    try:
        result = subprocess.run(
            [str(path)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    # Most tools exit non-zero with no args, but that still means they ran.
    # Um sinal (returncode negativo) indica binário incompatível/crash.
    return result.returncode >= 0


def _first_runnable(candidates: Iterable[Path]) -> Path | None:
    for candidate in candidates:
        if _runnable(candidate):
            return candidate
    return None


def _version_sort_key(name: str) -> tuple[int, ...]:
    """Comparação numérica: android-36 > android-9 e r30 > r9/r30c.

    Também lida com nomes como 'android-ndk-25.2.9519653'. O padding evita
    que um prefixo curto (36,) seja considerado menor que (36, 0, 0).
    """
    numbers = [int(piece) for piece in re.findall(r"\d+", name)][:8]
    return tuple(numbers) + (0,) * (8 - len(numbers))


@dataclass(frozen=True)
class Toolchain:
    sdk_dir: Path
    ndk_dir: Path
    build_tools_dir: Path
    platform_dir: Path
    aapt2: Path
    zipalign: Path
    apksigner: Path
    d8: Path | None
    java: Path
    keytool: Path
    abi: str
    api: int
    host_tag: str

    @property
    def ndk_toolchain_bin(self) -> Path:
        return self.ndk_dir / "toolchains" / "llvm" / "prebuilt" / self.host_tag / "bin"

    @property
    def sysroot(self) -> Path:
        return self.ndk_toolchain_bin.parent / "sysroot"

    def clang(self, cpp: bool = False) -> Path:
        prefix = ABI_CLANG_PREFIX[self.abi]
        name = f"{prefix}{self.api}-clang++" if cpp else f"{prefix}{self.api}-clang"
        path = self.ndk_toolchain_bin / name
        if _executable(path):
            return path
        # Fall back to unversioned clang with explicit --target.
        return self.ndk_toolchain_bin / ("clang++" if cpp else "clang")

    def clang_target_flags(self, *, explicit_target: bool = False) -> list[str]:
        """Flags de sysroot/target.

        Os wrappers versionados (ex.: aarch64-linux-android36-clang++) já
        embutem o target, então --target só é necessário com clang genérico.
        """
        flags: list[str] = []
        if explicit_target:
            triple = ABI_TRIPLES[self.abi]
            flags += ["-target", f"{triple}{self.api}"]
        flags.append(f"--sysroot={self.sysroot}")
        return flags

    def native_app_glue_dir(self) -> Path:
        return self.ndk_dir / "sources" / "android" / "native_app_glue"

    def for_abi(self, abi: str) -> "Toolchain":
        """Retorna uma cópia desta toolchain apontando para outra ABI."""
        if abi not in ABI_TRIPLES:
            raise RuntimeError(
                f"ABI não suportada: {abi}. Use uma de: {', '.join(ABI_TRIPLES)}"
            )
        return replace(self, abi=abi)

    def ndk_shared_lib(self, abi: str, name: str) -> Path | None:
        """Localiza uma lib compartilhada do sysroot para a ABI.

        Procura em <triple>/<name> e em <triple>/<api>/<name> (stubs de API,
        como libvulkan.so no NDK r30).
        """
        if abi not in ABI_TRIPLES:
            raise RuntimeError(f"ABI não suportada: {abi}")
        base = self.sysroot / "usr" / "lib" / ABI_TRIPLES[abi]
        candidates = [base / name]
        candidates += [base / str(api) / name for api in range(self.api, 20, -1)]
        for path in candidates:
            if path.is_file():
                return path
        return None

    def summary(self) -> str:
        return (
            f"SDK: {self.sdk_dir}\n"
            f"NDK: {self.ndk_dir}\n"
            f"build-tools: {self.build_tools_dir.name}\n"
            f"platform: {self.platform_dir.name}\n"
            f"ABI: {self.abi} (API {self.api})\n"
            f"aapt2: {self.aapt2}\n"
            f"zipalign: {self.zipalign}\n"
            f"apksigner: {self.apksigner}\n"
            f"clang: {self.clang()}"
        )


def _host_tag(ndk_dir: Path) -> str:
    prebuilt = ndk_dir / "toolchains" / "llvm" / "prebuilt"
    if not prebuilt.is_dir():
        raise RuntimeError(f"NDK prebuilt toolchain não encontrado em {prebuilt}")

    uname = getattr(os, "uname", None)
    machine = (uname().machine if uname else platform.machine()).lower()
    mapping = {
        "aarch64": "linux-arm64",
        "arm64": "linux-arm64",
        "armv8l": "linux-arm64",
        "x86_64": "linux-x86_64",
        "amd64": "linux-x86_64",
        "i686": "linux-x86",
        "x86": "linux-x86",
    }
    # Ordem explícita: um sorted() alphabetical escolheria darwin-* antes de
    # linux-* em NDKs multiplataforma.
    preferred = [mapping.get(machine), "linux-arm64", "linux-x86_64", "linux-x86"]
    candidates: list[str] = []
    for tag in preferred:
        if tag and tag not in candidates and (prebuilt / tag).is_dir():
            candidates.append(tag)
    candidates += sorted(
        p.name for p in prebuilt.iterdir()
        if p.is_dir() and p.name not in candidates
    )
    for tag in candidates:
        if _executable(prebuilt / tag / "bin" / "clang"):
            return tag
    raise RuntimeError(
        f"NDK prebuilt com bin/clang executável não encontrado em {prebuilt}; "
        f"candidatos: {', '.join(candidates) or 'nenhum'}"
    )


def _find_android_root() -> Path:
    override = os.environ.get("APKFAST_ANDROID_ROOT")
    if override:
        return Path(override).expanduser()
    return ANDROID_ROOT


def _find_sdk(android_root: Path, explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit).expanduser()
        if not path.is_dir():
            raise RuntimeError(f"SDK não encontrado: {path}")
        return path
    for name in ("android-sdk", "sdk", "Sdk", "android-sdk-linux"):
        path = android_root / name
        if path.is_dir():
            return path
    raise RuntimeError(f"Android SDK não encontrado em {android_root}")


def _find_ndk(android_root: Path, explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit).expanduser()
        if not path.is_dir():
            raise RuntimeError(f"NDK não encontrado: {path}")
        return path
    if not android_root.is_dir():
        raise RuntimeError(f"Diretório Android não encontrado: {android_root}")
    candidates = sorted(
        (p for p in android_root.iterdir() if p.name.startswith("android-ndk") and p.is_dir()),
        key=lambda p: (_version_sort_key(p.name), p.name),
        reverse=True,
    )
    if candidates:
        return candidates[0]
    raise RuntimeError(f"Android NDK não encontrado em {android_root}")


def _find_build_tools(sdk_dir: Path, explicit: str | None) -> Path:
    base = sdk_dir / "build-tools"
    if explicit:
        path = base / explicit
        if not path.is_dir():
            raise RuntimeError(f"build-tools não encontrado: {path}")
        return path
    if not base.is_dir():
        raise RuntimeError(f"Diretório build-tools não encontrado: {base}")
    required = ("aapt2", "zipalign", "apksigner")
    candidates = sorted(
        (p for p in base.iterdir() if p.is_dir()),
        key=lambda p: (_version_sort_key(p.name), p.name),
        reverse=True,
    )
    scored: list[tuple[int, Path]] = []
    for path in candidates:
        working = sum(_runnable(path / tool) for tool in required)
        if working == len(required):
            return path
        scored.append((working, path))
    if not candidates:
        raise RuntimeError(f"Nenhum build-tools instalado em {base}")
    # Nenhum diretório é totalmente executável (comum em Termux: binários
    # x86_64 no SDK). Mantém o fallback via PATH, mas escolhe o diretório com
    # mais ferramentas utilizáveis em vez de simplesmente o mais novo.
    best_count, best = max(
        scored, key=lambda item: (item[0], _version_sort_key(item[1].name))
    )
    if best_count == 0 and not any(shutil.which(tool) for tool in required):
        raise RuntimeError(
            f"build-tools sem ferramentas executáveis em {base}: "
            f"{', '.join(p.name for p in candidates)} "
            "(verifique a arquitetura dos binários ou instale aapt2/zipalign/apksigner no PATH)"
        )
    return best


def _find_platform(sdk_dir: Path, api: int) -> Path:
    base = sdk_dir / "platforms"
    candidates = [
        base / f"android-{api}-2",
        base / f"android-{api}",
    ]
    for path in candidates:
        if (path / "android.jar").is_file():
            return path
    # Fall back to highest API available with an android.jar.
    if not base.is_dir():
        raise RuntimeError(f"Diretório platforms não encontrado: {base}")
    available = sorted(
        (p for p in base.iterdir() if p.is_dir() and (p / "android.jar").is_file()),
        key=lambda p: (_version_sort_key(p.name), p.name),
        reverse=True,
    )
    if available:
        return available[0]
    raise RuntimeError(f"Plataforma android-{api} (android.jar) não encontrada em {base}")


def _find_tool(name: str, extra: Iterable[Path] = ()) -> Path:
    candidates = list(extra)
    found = shutil.which(name)
    if found:
        candidates.append(Path(found))
    path = _first_runnable(candidates)
    if path is None:
        raise RuntimeError(f"Ferramenta executável não encontrada: {name}")
    return path


def detect(
    *,
    abi: str = "arm64-v8a",
    api: int = 36,
    sdk_dir: str | None = None,
    ndk_dir: str | None = None,
    build_tools: str | None = None,
) -> Toolchain:
    if abi not in ABI_TRIPLES:
        raise RuntimeError(
            f"ABI não suportada: {abi}. Use uma de: {', '.join(ABI_TRIPLES)}"
        )
    if not isinstance(api, int) or api <= 0:
        raise RuntimeError(f"API Android inválida: {api!r}")
    android_root = _find_android_root()
    sdk = _find_sdk(android_root, sdk_dir)
    ndk = _find_ndk(android_root, ndk_dir)
    bt = _find_build_tools(sdk, build_tools)
    platform = _find_platform(sdk, api)

    aapt2 = _find_tool("aapt2", [bt / "aapt2"])
    zipalign = _find_tool("zipalign", [bt / "zipalign"])
    apksigner = _find_tool("apksigner", [bt / "apksigner"])
    d8 = _first_runnable([bt / "d8"]) or _first_runnable([bt.parent / "d8"])

    java = _find_tool("java")
    keytool = _find_tool("keytool")

    return Toolchain(
        sdk_dir=sdk,
        ndk_dir=ndk,
        build_tools_dir=bt,
        platform_dir=platform,
        aapt2=aapt2,
        zipalign=zipalign,
        apksigner=apksigner,
        d8=d8,
        java=java,
        keytool=keytool,
        abi=abi,
        api=api,
        host_tag=_host_tag(ndk),
    )
