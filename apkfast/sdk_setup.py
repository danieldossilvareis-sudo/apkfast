"""Instala Android SDK + NDK pré-compilados para Termux (aarch64-linux-android).

Fonte: https://github.com/HomuHomu833 (releases android-sdk-custom e android-ndk-custom).
Os arquivos são tar.xz já compilados para Android/Termux — não é necessário
sdkmanager, Java desktop nem Gradle (sdkmanager só busca a platform android-XX).
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import urllib.error
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
    except urllib.error.HTTPError as exc:
        raise SetupError(f"GitHub retornou HTTP {exc.code} para {url}") from exc
    except (OSError, ValueError) as exc:
        raise SetupError(f"Falha ao consultar GitHub: {exc}") from exc


def _release_tags(releases: list[object]) -> list[str]:
    tags: list[str] = []
    for release in releases:
        if isinstance(release, dict) and release.get("tag_name"):
            tag = str(release["tag_name"])
            if tag not in tags:
                tags.append(tag)
    return tags


def list_versions(repo: str, limit: int = 5) -> list[str]:
    if limit < 1:
        raise SetupError("limit deve ser >= 1")
    data = _fetch_json(f"https://api.github.com/repos/{repo}/releases?per_page={limit}")
    if not isinstance(data, list):
        raise SetupError("Resposta inesperada da API do GitHub")
    return _release_tags(data)


def _normalize_version(repo: str, version: str | None) -> str | None:
    """Aceita ``r30``/``30`` para NDK e ``37``/``37.0.0`` para SDK."""
    if version is None:
        return None
    value = str(version).strip()
    if not value:
        return None
    if "ndk" in repo and re.fullmatch(r"\d+[a-z]?", value, re.IGNORECASE):
        return "r" + value
    return value


def resolve_release(repo: str, version: str | None) -> Release:
    wanted = _normalize_version(repo, version)
    data = _fetch_json(f"https://api.github.com/repos/{repo}/releases?per_page=100")
    if not isinstance(data, list):
        raise SetupError("Resposta inesperada da API do GitHub")

    releases = [item for item in data if isinstance(item, dict)]
    if wanted:
        matches = [item for item in releases if str(item.get("tag_name", "")) == wanted]
        if not matches:
            # --sdk-version 37 deve resolver a release 37.0.0.
            matches = [
                item for item in releases
                if str(item.get("tag_name", "")).startswith(wanted + ".")
            ]
        if not matches:
            available = ", ".join(_release_tags(releases)[:10]) or "nenhuma"
            raise SetupError(
                f"Release {wanted} não encontrada em {repo}. Versões disponíveis: {available}"
            )
        releases = matches

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
        for asset in release.get("assets") or []:
            if not isinstance(asset, dict):
                continue
            name = str(asset.get("name") or "")
            match = expected_pattern.match(name)
            if not match:
                continue
            if "ndk" in repo:
                asset_version = match.group("version")
                if wanted:
                    if asset_version.lstrip("r").lower() != wanted.lstrip("r").lower():
                        continue
                elif re.search(r"beta|rc|alpha", asset_version, re.IGNORECASE):
                    # Sem versão explícita, pega a release estável.
                    continue
            url = asset.get("browser_download_url")
            if not url:
                continue
            try:
                size = int(asset.get("size", 0))
            except (TypeError, ValueError) as exc:
                raise SetupError(f"Tamanho inválido no asset {name}") from exc
            return Release(
                tag=str(release.get("tag_name") or wanted or ""),
                name=name,
                size=size,
                url=str(url),
                created_at=str(release.get("published_at") or ""),
            )
    available = ", ".join(_release_tags(releases)[:10]) or "nenhuma"
    raise SetupError(
        f"Nenhum asset {expected} encontrado nos releases de {repo}. "
        f"Versões disponíveis: {available}"
    )


def _download(url: str, destination: Path, quiet: bool = False) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_file():
        size = destination.stat().st_size
        if size > 0:
            if not quiet:
                print(f"[*] Usando cache: {destination} ({size // 1024 // 1024} MB)")
            return
        destination.unlink()
    elif destination.exists():
        raise SetupError(f"Caminho de download não é arquivo: {destination}")

    if not quiet:
        print(f"[*] Baixando {url}\n    -> {destination}")
    request = urllib.request.Request(url, headers={"User-Agent": "apkfast-setup-sdk"})
    tmp = destination.with_name(destination.name + ".part")
    downloaded = 0
    try:
        with urllib.request.urlopen(request, timeout=60) as response, tmp.open("wb") as output:
            try:
                total = int(response.headers.get("Content-Length") or 0)
            except (TypeError, ValueError):
                total = 0
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
    except (OSError, urllib.error.URLError) as exc:
        tmp.unlink(missing_ok=True)
        raise SetupError(f"Falha no download: {exc}") from exc

    if downloaded == 0:
        tmp.unlink(missing_ok=True)
        raise SetupError(f"Download vazio: {url}")
    try:
        tmp.replace(destination)
    except OSError as exc:
        tmp.unlink(missing_ok=True)
        raise SetupError(f"Falha ao salvar {destination}: {exc}") from exc


def _extract(archive: Path, target: Path, quiet: bool = False) -> None:
    if not shutil.which("tar"):
        raise SetupError("tar não encontrado")
    try:
        target.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise SetupError(f"Falha ao criar {target}: {exc}") from exc
    if not quiet:
        print(f"[*] Extraindo {archive.name} em {target} ...")
    try:
        result = subprocess.run(
            ["tar", "-xf", str(archive), "-C", str(target), "--strip-components=1"],
            check=False,
        )
    except OSError as exc:
        raise SetupError(f"Falha ao executar tar: {exc}") from exc
    if result.returncode != 0:
        raise SetupError(f"Falha ao extrair {archive.name}")


def _platform_ok(sdk_dir: Path, api: int) -> bool:
    platforms = sdk_dir / "platforms"
    return any(
        (platforms / name / "android.jar").is_file()
        for name in (f"android-{api}", f"android-{api}-2")
    )


def _accept_licenses(sdkmanager: Path, sdk_dir: Path, quiet: bool) -> bool:
    """Aceita licenças antes da instalação (o sdkmanager pode pedir várias)."""
    try:
        result = subprocess.run(
            [str(sdkmanager), f"--sdk_root={sdk_dir}", "--licenses"],
            input="y\n" * 50,
            text=True,
            stdout=subprocess.DEVNULL if quiet else None,
            stderr=subprocess.DEVNULL if quiet else None,
            timeout=300,
        )
    except subprocess.TimeoutExpired:
        return False
    except OSError as exc:
        raise SetupError(f"Falha ao executar sdkmanager --licenses: {exc}") from exc
    return result.returncode == 0


def _install_platform(sdk_dir: Path, api: int, quiet: bool = False) -> None:
    """Instala platforms/android-XX (android.jar) via sdkmanager do próprio SDK.

    Os releases customizados do HomuHomu833 trazem build-tools/platform-tools/
    cmdline-tools, mas não platforms/. O sdkmanager funciona normalmente no
    Termux e busca o pacote oficial do Google.
    """
    if api < 1:
        raise SetupError(f"API da platform inválida: {api}")
    if _platform_ok(sdk_dir, api):
        if not quiet:
            print(f"[*] Platform android-{api} já instalada.")
        return

    sdkmanager = sdk_dir / "cmdline-tools" / "latest" / "bin" / "sdkmanager"
    if not sdkmanager.is_file():
        # fallback para versões antigas do layout
        sdkmanager = sdk_dir / "cmdline-tools" / "bin" / "sdkmanager"
    if not sdkmanager.is_file():
        raise SetupError(f"sdkmanager não encontrado em {sdk_dir}")

    if not _accept_licenses(sdkmanager, sdk_dir, quiet) and not quiet:
        print("[!] Não foi possível aceitar licenças separadamente; tentando na instalação.")
    if not quiet:
        print(f"[*] Instalando platforms;android-{api} via sdkmanager ...")
    try:
        result = subprocess.run(
            [str(sdkmanager), f"--sdk_root={sdk_dir}", f"platforms;android-{api}"],
            input="y\n" * 50,
            text=True,
            stdout=subprocess.DEVNULL if quiet else None,
            stderr=subprocess.DEVNULL if quiet else None,
            timeout=600,
        )
    except subprocess.TimeoutExpired as exc:
        raise SetupError("Timeout ao instalar platform via sdkmanager") from exc
    except OSError as exc:
        raise SetupError(f"Falha ao executar sdkmanager: {exc}") from exc
    if result.returncode != 0:
        raise SetupError(
            f"sdkmanager falhou ao instalar platforms;android-{api}. "
            "Verifique licenças e conexão de rede."
        )
    if not _platform_ok(sdk_dir, api):
        raise SetupError(f"sdkmanager não gerou android.jar para android-{api}")


def _ndk_dir_name(value: str | Release | None) -> str:
    """Deriva android-ndk-rXX da release (ou da versão pedida), nunca r30 fixo."""
    raw = value.tag or value.name if isinstance(value, Release) else str(value or "")
    match = re.search(r"r(\d+[a-z]?)", str(raw), re.IGNORECASE)
    if not match:
        raise SetupError(f"Não foi possível derivar a versão do NDK de {raw!r}")
    version = "r" + match.group(1)
    if not re.fullmatch(r"[A-Za-z0-9._-]+", version):
        raise SetupError(f"Versão do NDK inválida: {version}")
    return f"android-ndk-{version}"


def _existing_ndk(root: Path) -> Path | None:
    candidates = [p for p in root.glob("android-ndk-*") if p.is_dir()]

    def sort_key(path: Path) -> int:
        match = re.search(r"(\d+)", path.name)
        return int(match.group(1)) if match else 0

    candidates.sort(key=sort_key, reverse=True)
    return candidates[0] if candidates else None


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
    if root.exists() and not root.is_dir():
        raise SetupError(f"Diretório base não é um diretório: {root}")
    sdk_dir = root / "android-sdk"
    wanted_sdk = _normalize_version(SDK_REPO, sdk_version)
    wanted_ndk = _normalize_version(NDK_REPO, ndk_version)

    # Caminho rápido: sem versões explícitas, não consulta a rede para dizer que
    # já está instalado. Com --ndk-version, o nome do diretório vem da versão.
    if not force:
        existing_ndk = _existing_ndk(root) if not wanted_ndk else (
            root / _ndk_dir_name(wanted_ndk) if (root / _ndk_dir_name(wanted_ndk)).is_dir() else None
        )
        if sdk_dir.is_dir() and existing_ndk is not None:
            if existing_ndk.is_dir():
                if not quiet:
                    print(f"[✓] SDK e NDK já instalados em {root}")
                    print(f"    {sdk_dir}")
                    print(f"    {existing_ndk}")
                    print("    Use --force para reinstalar.")
                return {"sdk": sdk_dir, "ndk": existing_ndk}

    sdk_release = resolve_release(SDK_REPO, wanted_sdk)
    ndk_release = resolve_release(NDK_REPO, wanted_ndk)
    ndk_dir = root / _ndk_dir_name(ndk_release)
    # ~50 MB extras para platforms/android-XX via sdkmanager
    total_mb = (sdk_release.size + ndk_release.size) // 1024 // 1024 + 50

    if not assume_yes and not quiet:
        print("[*] apkfast setup-sdk")
        print(f"    SDK: {sdk_release.tag}  ({sdk_release.size // 1024 // 1024} MB)")
        print(f"    NDK: {ndk_release.tag}  ({ndk_release.size // 1024 // 1024} MB)")
        print(f"    Total: ~{total_mb} MB em {root}")
        try:
            answer = input("Continuar? [S/n]: ").strip().lower()
        except EOFError as exc:
            raise SetupError("Confirmação não recebida; use -y/--yes") from exc
        if answer.startswith("n"):
            raise SetupError("Instalação cancelada")

    archives = root / ".apkfast-downloads"
    sdk_archive = archives / sdk_release.name
    ndk_archive = archives / ndk_release.name

    _download(sdk_release.url, sdk_archive, quiet)
    _download(ndk_release.url, ndk_archive, quiet)

    # Backup dos diretórios que serão substituídos: em caso de falha, restaura
    # a instalação anterior; diretórios pré-existentes sem --force são mantidos.
    backups: list[tuple[Path, Path]] = []
    created = {sdk_dir: not sdk_dir.exists(), ndk_dir: not ndk_dir.exists()}
    try:
        for directory in (sdk_dir, ndk_dir):
            if not directory.exists():
                continue
            if not directory.is_dir():
                raise SetupError(f"Caminho não é um diretório: {directory}")
            if force:
                backup = directory.with_name(
                    f"{directory.name}.apkfast-backup-{os.getpid()}"
                )
                if backup.exists():
                    raise SetupError(f"Backup temporário já existe: {backup}")
                directory.rename(backup)
                backups.append((backup, directory))

        _extract(sdk_archive, sdk_dir, quiet)
        _extract(ndk_archive, ndk_dir, quiet)
        _install_platform(sdk_dir, platform_api, quiet)
        _validate(sdk_dir, ndk_dir, platform_api)
    except (SetupError, OSError) as exc:
        for directory in (sdk_dir, ndk_dir):
            if directory.is_dir() and (created[directory] or any(
                original == directory for _, original in backups
            )):
                shutil.rmtree(directory, ignore_errors=True)
        for backup, original in reversed(backups):
            if backup.is_dir():
                if original.exists():
                    shutil.rmtree(original, ignore_errors=True)
                backup.rename(original)
        if isinstance(exc, SetupError):
            raise
        raise SetupError(f"Falha ao instalar SDK/NDK: {exc}") from exc
    else:
        for backup, _ in backups:
            shutil.rmtree(backup, ignore_errors=True)

    if not quiet:
        print(f"[✓] SDK e NDK instalados em {root}")
    return {"sdk": sdk_dir, "ndk": ndk_dir}


def _validate(sdk_dir: Path, ndk_dir: Path, platform_api: int = 36) -> None:
    problems = []
    if not (sdk_dir / "build-tools").is_dir():
        problems.append(f"build-tools ausente em {sdk_dir}")
    if not _platform_ok(sdk_dir, platform_api):
        problems.append(f"android.jar de android-{platform_api} ausente em {sdk_dir}")
    if not (ndk_dir / "toolchains" / "llvm" / "prebuilt").is_dir():
        problems.append(f"toolchains/llvm/prebuilt ausente em {ndk_dir}")
    if not (ndk_dir / "build" / "cmake" / "android.toolchain.cmake").is_file():
        problems.append(f"android.toolchain.cmake ausente em {ndk_dir}")
    if problems:
        raise SetupError("Instalação incompleta: " + "; ".join(problems))
