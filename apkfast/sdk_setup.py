"""Instala Android SDK + NDK pré-compilados para Termux (aarch64-linux-android).

Fonte: https://github.com/HomuHomu833 (releases android-sdk-custom e android-ndk-custom).
Os arquivos são tar.xz já compilados para Android/Termux — não é necessário
sdkmanager, Java desktop nem Gradle.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import urllib.request
from dataclasses import dataclass
from pathlib import Path

HOME = Path(os.environ.get("HOME", str(Path.home())))
SDK_REPO = "HomuHomu833/android-sdk-custom"
NDK_REPO = "HomuHomu833/android-ndk-custom"
TARGET_TRIPLE = "aarch64-linux-android"


class SetupError(RuntimeError):
    pass


@dataclass(frozen=True)
class Release:
    tag: str
    name: str
    size: int
    url: str
    created_at: str


def _fetch_json(url: str) -> object:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "apkfast-setup-sdk",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return json.load(response)
    except Exception as exc:
        raise SetupError(f"Falha ao consultar GitHub: {exc}") from exc


def list_versions(repo: str, limit: int = 5) -> list[str]:
    data = _fetch_json(f"https://api.github.com/repos/{repo}/releases?per_page={limit}")
    if not isinstance(data, list):
        raise SetupError("Resposta inesperada da API do GitHub")
    return [str(release.get("tag_name", "")) for release in data if release.get("tag_name")]


def resolve_release(repo: str, version: str | None) -> Release:
    if version:
        url = f"https://api.github.com/repos/{repo}/releases/tags/{version}"
        data = _fetch_json(url)
        if not isinstance(data, dict) or "assets" not in data:
            raise SetupError(f"Release {version} não encontrada em {repo}")
        releases = [data]
    else:
        url = f"https://api.github.com/repos/{repo}/releases?per_page=30"
        data = _fetch_json(url)
        if not isinstance(data, list):
            raise SetupError("Resposta inesperada da API do GitHub")
        releases = data

    if "sdk" in repo:
        # android-sdk-aarch64-linux-android.tar.xz
        expected_pattern = re.compile(rf"^android-sdk-{re.escape(TARGET_TRIPLE)}\.tar\.xz$")
        expected = f"android-sdk-{TARGET_TRIPLE}.tar.xz"
    else:
        # android-ndk-r30-aarch64-linux-android.tar.xz (versão no meio do nome)
        expected_pattern = re.compile(
            rf"^android-ndk-(?P<version>[^-]+)-{re.escape(TARGET_TRIPLE)}\.tar\.xz$"
        )
        expected = f"android-ndk-<versão>-{TARGET_TRIPLE}.tar.xz"

    for release in releases:
        for asset in release.get("assets", []):
            if expected_pattern.match(str(asset.get("name", ""))):
                name = str(asset["name"])
                if "ndk" in repo:
                    matched_version = expected_pattern.match(name).group("version")
                    if version and matched_version.lstrip("r") != version.lstrip("r"):
                        continue
                return Release(
                    tag=str(release.get("tag_name", "")),
                    name=str(asset["name"]),
                    size=int(asset["size"]),
                    url=str(asset["browser_download_url"]),
                    created_at=str(release.get("published_at", "")),
                )
    raise SetupError(
        f"Nenhum asset {expected} encontrado nos releases de {repo}. "
        f"Versões disponíveis: {', '.join(list_versions(repo))}"
    )


def _download(url: str, destination: Path, quiet: bool = False) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        size = destination.stat().st_size
        if size > 0:
            if not quiet:
                print(f"[*] Usando cache: {destination} ({size // 1024 // 1024} MB)")
            return

    if not quiet:
        print(f"[*] Baixando {url}\n    -> {destination}")
    request = urllib.request.Request(url, headers={"User-Agent": "apkfast-setup-sdk"})
    tmp = destination.with_suffix(destination.suffix + ".part")
    try:
        with urllib.request.urlopen(request, timeout=60) as response, tmp.open("wb") as output:
            total = int(response.headers.get("Content-Length", 0))
            downloaded = 0
            last_percent = -1
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
                downloaded += len(chunk)
                if total and not quiet:
                    percent = downloaded * 100 // total
                    if percent != last_percent and percent % 10 == 0:
                        print(f"    {percent:3d}% ({downloaded // 1024 // 1024} MB)", flush=True)
                        last_percent = percent
        tmp.replace(destination)
    except Exception as exc:
        if tmp.exists():
            tmp.unlink()
        raise SetupError(f"Falha no download: {exc}") from exc


def _extract(archive: Path, target: Path, quiet: bool = False) -> None:
    if not shutil.which("tar"):
        raise SetupError("tar não encontrado")
    target.mkdir(parents=True, exist_ok=True)
    if not quiet:
        print(f"[*] Extraindo {archive.name} em {target} ...")
    result = subprocess.run(
        ["tar", "-xf", str(archive), "-C", str(target), "--strip-components=1"]
    )
    if result.returncode != 0:
        raise SetupError(f"Falha ao extrair {archive.name}")


def _install_platform(sdk_dir: Path, api: int, quiet: bool = False) -> None:
    """Instala platforms/android-XX (android.jar) via sdkmanager do próprio SDK.

    Os releases customizados do HomuHomu833 trazem build-tools/platform-tools/
    cmdline-tools, mas não platforms/. O sdkmanager funciona normalmente no
    Termux e busca o pacote oficial do Google.
    """
    if (sdk_dir / "platforms" / f"android-{api}" / "android.jar").is_file():
        if not quiet:
            print(f"[*] Platform android-{api} já instalada.")
        return

    sdkmanager = sdk_dir / "cmdline-tools" / "latest" / "bin" / "sdkmanager"
    if not sdkmanager.is_file():
        # fallback para versões antigas do layout
        sdkmanager = sdk_dir / "cmdline-tools" / "bin" / "sdkmanager"
    if not sdkmanager.is_file():
        raise SetupError(f"sdkmanager não encontrado em {sdk_dir}")

    if not quiet:
        print(f"[*] Instalando platforms;android-{api} via sdkmanager ...")
    try:
        result = subprocess.run(
            [str(sdkmanager), f"--sdk_root={sdk_dir}", f"platforms;android-{api}"],
            input="y\ny\ny\n",
            text=True,
            stdout=subprocess.DEVNULL if quiet else None,
            stderr=subprocess.DEVNULL if quiet else None,
            timeout=600,
        )
    except subprocess.TimeoutExpired as exc:
        raise SetupError("Timeout ao instalar platform via sdkmanager") from exc
    if result.returncode != 0:
        raise SetupError(f"sdkmanager falhou ao instalar platforms;android-{api}")


def install_sdk(
    *,
    target: Path | None = None,
    sdk_version: str | None = None,
    ndk_version: str | None = None,
    platform_api: int = 36,
    force: bool = False,
    quiet: bool = False,
    assume_yes: bool = False,
) -> dict[str, Path]:
    root = (target or (HOME / "Android")).expanduser().resolve()
    sdk_dir = root / "android-sdk"
    ndk_dir = root / "android-ndk-r30"

    if not force and sdk_dir.is_dir() and ndk_dir.is_dir():
        if not quiet:
            print(f"[✓] SDK e NDK já instalados em {root}")
            print(f"    {sdk_dir}")
            print(f"    {ndk_dir}")
            print("    Use --force para reinstalar.")
        return {"sdk": sdk_dir, "ndk": ndk_dir}

    sdk_release = resolve_release(SDK_REPO, sdk_version)
    ndk_release = resolve_release(NDK_REPO, ndk_version)
    # ~50 MB extras para platforms/android-XX via sdkmanager
    total_mb = (sdk_release.size + ndk_release.size) // 1024 // 1024 + 50

    if not assume_yes and not quiet:
        print("[*] apkfast setup-sdk")
        print(f"    SDK: {sdk_release.tag}  ({sdk_release.size // 1024 // 1024} MB)")
        print(f"    NDK: {ndk_release.tag}  ({ndk_release.size // 1024 // 1024} MB)")
        print(f"    Total: ~{total_mb} MB em {root}")
        answer = input("Continuar? [S/n]: ").strip().lower()
        if answer.startswith("n"):
            raise SetupError("Instalação cancelada")

    archives = root / ".apkfast-downloads"
    sdk_archive = archives / sdk_release.name
    ndk_archive = archives / ndk_release.name

    _download(sdk_release.url, sdk_archive, quiet)
    _download(ndk_release.url, ndk_archive, quiet)

    sdk_existed = sdk_dir.is_dir()
    ndk_existed = ndk_dir.is_dir()
    if force:
        for directory in (sdk_dir, ndk_dir):
            if directory.is_dir():
                shutil.rmtree(directory)

    try:
        _extract(sdk_archive, sdk_dir, quiet)

        # O release do NDK já vem com o nome android-ndk-r30 dentro do tar;
        # --strip-components=1 joga tudo direto em ndk_dir.
        ndk_dir.mkdir(parents=True, exist_ok=True)
        _extract(ndk_archive, ndk_dir, quiet)
        _install_platform(sdk_dir, platform_api, quiet)
        _validate(sdk_dir, ndk_dir)
    except SetupError:
        # Remove apenas diretórios criados por esta execução.
        if not sdk_existed and sdk_dir.is_dir():
            shutil.rmtree(sdk_dir, ignore_errors=True)
        if not ndk_existed and ndk_dir.is_dir():
            shutil.rmtree(ndk_dir, ignore_errors=True)
        raise
    if not quiet:
        print(f"[✓] SDK e NDK instalados em {root}")
    return {"sdk": sdk_dir, "ndk": ndk_dir}


def _validate(sdk_dir: Path, ndk_dir: Path) -> None:
    problems = []
    if not (sdk_dir / "build-tools").is_dir():
        problems.append(f"build-tools ausente em {sdk_dir}")
    if not (sdk_dir / "platforms").is_dir():
        problems.append(f"platforms ausente em {sdk_dir}")
    if not (ndk_dir / "toolchains" / "llvm" / "prebuilt").is_dir():
        problems.append(f"toolchains/llvm/prebuilt ausente em {ndk_dir}")
    if problems:
        raise SetupError("Instalação incompleta: " + "; ".join(problems))
