"""Suporte a Android App Bundle (AAB) sem Gradle.

O fluxo é deliberadamente separado do pipeline de APK:

1. ``aapt2 link --proto-format`` (feito pelo builder) gera um APK temporário
   com manifesto/resource table em protobuf.
2. :func:`build_aab` monta o módulo ``base.zip`` e chama
   ``bundletool build-bundle``.
3. :func:`sign_aab` usa ``jarsigner`` (AAB é v1/JAR signed; ``apksigner`` não
   serve para AAB).
4. :func:`verify_aab` valida o bundle e, opcionalmente, gera um APK set
   universal com ``bundletool build-apks``.

O jar do bundletool fica em ``~/.apkfast/tools/`` (fora do repositório). A
versão padrão é fixa e o SHA256 é verificado antes de cada uso.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

BUNDLETOOL_VERSION = "1.18.3"
# GitHub release 1.18.3, asset bundletool-all-1.18.3.jar (digest publicado
# pelo próprio release: sha256:a099...). Ver docs/aab.md.
BUNDLETOOL_SHA256 = "a099cfa1543f55593bc2ed16a70a7c67fe54b1747bb7301f37fdfd6d91028e29"
BUNDLETOOL_REPO = "google/bundletool"

KNOWN_ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")

_VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")
_SHA_RE = re.compile(r"^[0-9a-fA-F]{64}$")
_TEXT_VERSION_RE = re.compile(r"\b(\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?)\b")
_DEX_RE = re.compile(r"^classes(?:[2-9]\d*)?\.dex$")


class BundleError(RuntimeError):
    """Falha de configuração, download, empacotamento, assinatura ou verificação."""


@dataclass(frozen=True)
class VerificationResult:
    manifest_xml: str
    validate_output: str
    apks_path: Path | None = None


def validate_version(version: str) -> str:
    """Aceita apenas ``X.Y.Z`` (evita path traversal no nome do jar)."""
    value = str(version).strip()
    if not _VERSION_RE.fullmatch(value):
        raise BundleError(
            f"Versão de bundletool inválida: {version!r}. Use X.Y.Z (ex.: {BUNDLETOOL_VERSION})."
        )
    return value


def asset_name(version: str) -> str:
    return f"bundletool-all-{validate_version(version)}.jar"


def bundletool_url(version: str) -> str:
    tag = validate_version(version)
    return (
        f"https://github.com/{BUNDLETOOL_REPO}/releases/download/"
        f"{tag}/{asset_name(tag)}"
    )


def tools_dir() -> Path:
    override = os.environ.get("APKFAST_TOOLS_DIR")
    return Path(override).expanduser() if override else Path.home() / ".apkfast" / "tools"


def bundletool_path(version: str = BUNDLETOOL_VERSION, directory: Path | None = None) -> Path:
    return (directory or tools_dir()) / asset_name(version)


def parse_version(text: str) -> str:
    """Extrai a versão da saída de ``bundletool version``."""
    match = _TEXT_VERSION_RE.search(str(text))
    if match is None:
        raise BundleError(f"Não foi possível ler a versão do bundletool em: {text!r}")
    return match.group(1)


def default_aab_path(
    package: str,
    suffix: str,
    dist_dir: Path,
    *,
    apk_output: Path | None = None,
    explicit: Path | None = None,
) -> Path:
    """Nome padrão do AAB espelhando o APK (<pacote>-<suffix>.<ext>)."""
    if explicit is not None:
        return Path(explicit).expanduser()
    if apk_output is not None:
        return Path(apk_output).expanduser().with_suffix(".aab")
    leaf = str(package).rsplit(".", 1)[-1] or "app"
    return Path(dist_dir).expanduser() / f"{leaf}-{suffix}.aab"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _expected_sha256(version: str, sha256: str | None = None) -> str:
    candidate = (sha256 or os.environ.get("APKFAST_BUNDLETOOL_SHA256") or "").strip().lower()
    if candidate:
        if not _SHA_RE.fullmatch(candidate):
            raise BundleError("SHA256 do bundletool deve ter 64 dígitos hexadecimais.")
        return candidate
    if version == BUNDLETOOL_VERSION:
        return BUNDLETOOL_SHA256

    # Versão alternativa: o digest vem do próprio release do GitHub (campos
    # ``digest`` dos assets). A URL é fixa e autenticada por TLS; se o campo
    # não existir, falhamos em vez de aceitar um jar sem checksum.
    url = f"https://api.github.com/repos/{BUNDLETOOL_REPO}/releases/tags/{version}"
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "apkfast-bundletool",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            payload = json.load(response)
    except (OSError, urllib.error.URLError, ValueError) as exc:
        raise BundleError(
            f"Não foi possível obter o SHA256 da versão {version} do bundletool: {exc}. "
            f"Defina APKFAST_BUNDLETOOL_SHA256 ou use --bundletool-version {BUNDLETOOL_VERSION}."
        ) from exc
    wanted = asset_name(version)
    for asset in payload.get("assets", []) if isinstance(payload, dict) else []:
        if not isinstance(asset, dict) or asset.get("name") != wanted:
            continue
        digest = str(asset.get("digest") or "")
        if digest.startswith("sha256:"):
            digest = digest.split(":", 1)[1].lower()
            if _SHA_RE.fullmatch(digest):
                return digest
    raise BundleError(
        f"Release {version} não publicou um digest SHA256 para {wanted}. "
        "Use --bundletool-version padrão ou defina APKFAST_BUNDLETOOL_SHA256."
    )


def require_java() -> Path:
    found = shutil.which("java")
    if not found:
        raise BundleError(
            "Java não encontrado no PATH. bundletool e jarsigner exigem um JDK; "
            "no Termux: pkg install openjdk-21"
        )
    return Path(found)


def require_jarsigner(explicit: Path | str | None = None) -> Path:
    if explicit is not None:
        path = Path(explicit).expanduser()
        if not path.is_file() or not os.access(path, os.X_OK):
            raise BundleError(f"jarsigner não é executável: {path}")
        return path
    found = shutil.which("jarsigner")
    if not found:
        raise BundleError(
            "jarsigner não encontrado no PATH; AAB exige assinatura v1/JAR. "
            "Instale um JDK (no Termux: pkg install openjdk-21). apksigner NÃO assina AAB."
        )
    return Path(found)


def ensure_bundletool(
    version: str = BUNDLETOOL_VERSION,
    quiet: bool = False,
    *,
    sha256: str | None = None,
    directory: Path | None = None,
) -> Path:
    """Baixa/valida o jar e devolve o caminho em cache."""
    tag = validate_version(version)
    expected = _expected_sha256(tag, sha256)
    target = bundletool_path(tag, directory)
    target.parent.mkdir(parents=True, exist_ok=True)

    if target.is_file():
        actual = _sha256_file(target)
        if actual == expected:
            if not quiet:
                print(f"[*] Usando bundletool {tag} (cache): {target}", flush=True)
            return target
        if not quiet:
            print(
                f"[!] SHA256 do bundletool em cache diverge ({actual}); baixando novamente.",
                flush=True,
            )
        target.unlink(missing_ok=True)
    elif target.exists():
        raise BundleError(f"Caminho de cache do bundletool não é arquivo: {target}")

    url = bundletool_url(tag)
    if not quiet:
        print(f"[*] Baixando bundletool {tag}:\n    {url}\n -> {target}", flush=True)
    part = target.with_name(target.name + ".part")
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "apkfast-bundletool"})
        with urllib.request.urlopen(request, timeout=120) as response, part.open("wb") as output:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
    except (OSError, urllib.error.URLError) as exc:
        part.unlink(missing_ok=True)
        raise BundleError(f"Falha ao baixar bundletool {tag}: {exc}") from exc

    actual = _sha256_file(part)
    if actual != expected:
        part.unlink(missing_ok=True)
        raise BundleError(
            f"SHA256 inválido para bundletool {tag}:\n"
            f"  esperado: {expected}\n  recebido: {actual}\n"
            "O download foi descartado."
        )
    try:
        part.replace(target)
    except OSError as exc:
        part.unlink(missing_ok=True)
        raise BundleError(f"Falha ao salvar {target}: {exc}") from exc
    return target


def _tail(text: str, limit: int = 40) -> str:
    lines = str(text).rstrip().splitlines()
    if len(lines) <= limit:
        return "\n".join(lines)
    return f"... ({len(lines) - limit} linha(s) omitida(s))\n" + "\n".join(lines[-limit:])


def _run_command(
    cmd: list[object], *, quiet: bool = False, capture: bool = False
) -> subprocess.CompletedProcess:
    capture = capture or quiet
    try:
        return subprocess.run(
            [str(part) for part in cmd],
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
            text=True,
        )
    except OSError as exc:
        raise BundleError(f"Não foi possível executar '{cmd[0]}': {exc}") from exc


def _bundletool_command(bundletool: Path, java: Path | None = None) -> list[Path]:
    jar = Path(bundletool).expanduser()
    if not jar.is_file():
        raise BundleError(
            f"bundletool não encontrado: {jar}. Rode ensure_bundletool() antes."
        )
    return [java or require_java(), "-jar", jar]


def run_bundletool(
    bundletool: Path,
    args: list[str],
    *,
    quiet: bool = False,
    capture: bool = False,
    java: Path | None = None,
) -> subprocess.CompletedProcess:
    cmd = _bundletool_command(bundletool, java) + args
    result = _run_command(cmd, quiet=quiet, capture=capture)
    if result.returncode != 0:
        output = _tail(result.stdout or "") if (capture or quiet) else ""
        raise BundleError(
            f"bundletool falhou ({result.returncode}): {' '.join(str(c) for c in cmd)}"
            + (f"\n{output}" if output else "")
        )
    return result


def _relative(name: str) -> str:
    path = Path(name)
    if not name or name.endswith("/") or path.is_absolute() or ".." in path.parts or "\\" in name:
        raise BundleError(f"Entrada de ZIP insegura no bundle: {name!r}")
    return path.as_posix()


def prepare_module(
    *,
    work: Path,
    proto_apk: Path,
    dex_dir: Path | None = None,
    lib_dir: Path | None = None,
    assets_dir: Path | None = None,
) -> Path:
    """Monta ``base.zip`` no formato exigido por ``bundletool build-bundle``."""
    if not proto_apk.is_file():
        raise BundleError(f"APK proto não encontrado: {proto_apk}")
    stage = work / "aab" / "module"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)

    try:
        with zipfile.ZipFile(proto_apk) as src:
            names = src.namelist()
            for required in ("AndroidManifest.xml", "resources.pb"):
                if required not in names:
                    raise BundleError(
                        f"APK proto {proto_apk} não contém {required}. "
                        "O aapt2 usado precisa suportar --proto-format."
                    )
            for info in src.infolist():
                name = _relative(info.filename)
                if name.endswith("/") or info.is_dir():
                    continue
                if name in ("AndroidManifest.xml", "resources.pb"):
                    destination = (
                        stage / "manifest" / "AndroidManifest.xml"
                        if name == "AndroidManifest.xml"
                        else stage / "resources.pb"
                    )
                elif name.startswith(("res/", "root/")):
                    destination = stage / name
                elif name.startswith("assets/") and assets_dir is None:
                    destination = stage / name
                else:
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(src.read(info.filename))
    except (OSError, zipfile.BadZipFile) as exc:
        raise BundleError(f"Falha ao ler APK proto {proto_apk}: {exc}") from exc

    if dex_dir is not None:
        dex_path = Path(dex_dir)
        if dex_path.is_dir():
            for dex in sorted(dex_path.glob("classes*.dex")):
                if not _DEX_RE.fullmatch(dex.name):
                    raise BundleError(f"Nome de DEX inesperado no AAB: {dex.name}")
                target = stage / "dex" / dex.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dex, target)

    if lib_dir is not None:
        lib_path = Path(lib_dir)
        if lib_path.is_dir():
            for lib in sorted(lib_path.rglob("*.so")):
                relative = lib.relative_to(lib_path).as_posix()
                abi = relative.split("/", 1)[0]
                if abi not in KNOWN_ABIS or "/" not in relative:
                    raise BundleError(
                        f"Biblioteca fora do layout lib/<abi>/: {relative}. "
                        f"ABIs aceitas: {', '.join(KNOWN_ABIS)}"
                    )
                target = stage / "lib" / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(lib, target)

    if assets_dir is not None:
        assets_path = Path(assets_dir).expanduser()
        if not assets_path.is_dir():
            raise BundleError(f"Diretório de assets não encontrado: {assets_path}")
        for asset in sorted(assets_path.rglob("*")):
            if not asset.is_file():
                continue
            relative = asset.relative_to(assets_path).as_posix()
            target = stage / "assets" / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(asset, target)

    base_zip = work / "aab" / "base.zip"
    try:
        with zipfile.ZipFile(base_zip, "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(stage.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(stage).as_posix())
    except OSError as exc:
        raise BundleError(f"Falha ao criar base.zip: {exc}") from exc
    return base_zip


def build_aab(
    *,
    work: Path,
    proto_apk: Path,
    bundletool: Path,
    dex_dir: Path | None = None,
    lib_dir: Path | None = None,
    assets_dir: Path | None = None,
    output: Path | None = None,
    quiet: bool = False,
    java: Path | None = None,
) -> Path:
    """Monta o módulo base e gera o AAB ainda não assinado."""
    base = prepare_module(
        work=work,
        proto_apk=proto_apk,
        dex_dir=dex_dir,
        lib_dir=lib_dir,
        assets_dir=assets_dir,
    )
    aab = Path(output).expanduser() if output is not None else Path(work) / "aab" / "unsigned.aab"
    aab.parent.mkdir(parents=True, exist_ok=True)
    run_bundletool(
        bundletool,
        ["build-bundle", f"--modules={base}", f"--output={aab}"],
        quiet=quiet,
        java=java,
    )
    if not aab.is_file() or aab.stat().st_size == 0:
        raise BundleError(f"bundletool não gerou um AAB válido em {aab}")
    return aab


def sign_aab(
    aab: Path,
    *,
    keystore: Path,
    store_password: str,
    key_alias: str,
    key_password: str,
    jarsigner: Path | str | None = None,
    output: Path | None = None,
    quiet: bool = False,
) -> Path:
    """Assina o AAB com jarsigner (v1/JAR; apksigner não suporta AAB)."""
    signer = require_jarsigner(jarsigner)
    require_java()
    source = Path(aab).expanduser()
    if not source.is_file():
        raise BundleError(f"AAB não encontrado para assinar: {source}")
    keystore = Path(keystore).expanduser()
    if not keystore.is_file():
        raise BundleError(f"Keystore não encontrado: {keystore}")

    signed = Path(output).expanduser() if output is not None else source.with_name(
        source.stem + "-signed.aab"
    )
    signed.parent.mkdir(parents=True, exist_ok=True)
    if signed.resolve() == source.resolve():
        source_copy = source.with_name(source.stem + ".unsigned.aab")
        shutil.copy2(source, source_copy)
        source = source_copy
    signed.unlink(missing_ok=True)

    cmd = [
        signer,
        "-keystore", keystore,
        "-storepass", store_password,
        "-keypass", key_password,
        "-digestalg", "SHA-256",
        "-signedjar", signed,
        source,
        key_alias,
    ]
    result = _run_command(cmd, quiet=quiet, capture=True)
    if result.returncode != 0:
        raise BundleError(
            f"jarsigner falhou ({result.returncode}):\n{_tail(result.stdout or '')}"
        )
    verify = _run_command([signer, "-verify", signed], quiet=True, capture=True)
    if verify.returncode != 0:
        raise BundleError(
            f"Assinatura do AAB não validou:\n{_tail(verify.stdout or '')}"
        )
    if not quiet and result.stdout:
        for line in result.stdout.splitlines():
            if "signed" in line.lower() or "warning" in line.lower():
                print(f"    {line}", flush=True)
    return signed


def verify_aab(
    aab: Path,
    *,
    bundletool: Path,
    work: Path | None = None,
    aapt2: Path | None = None,
    keystore: Path | None = None,
    store_password: str = "",
    key_alias: str = "",
    key_password: str = "",
    build_apks: bool = True,
    quiet: bool = False,
    java: Path | None = None,
) -> VerificationResult:
    """Valida o bundle e (por padrão) gera um APK set universal de teste."""
    bundle = Path(aab).expanduser()
    if not bundle.is_file() or not zipfile.is_zipfile(bundle):
        raise BundleError(f"AAB inválido (não é um ZIP): {bundle}")

    validate = run_bundletool(
        bundletool, ["validate", f"--bundle={bundle}"], quiet=quiet, capture=True, java=java
    )
    dumped = run_bundletool(
        bundletool,
        ["dump", "manifest", f"--bundle={bundle}"],
        quiet=quiet,
        capture=True,
        java=java,
    )
    manifest = dumped.stdout or ""
    if "<manifest" not in manifest or "package=" not in manifest:
        raise BundleError(
            "bundletool dump manifest não retornou um manifesto válido:\n"
            + _tail(manifest)
        )

    apks_path: Path | None = None
    if build_apks:
        aapt = Path(aapt2).expanduser() if aapt2 is not None else None
        if aapt is None:
            found = shutil.which("aapt2")
            aapt = Path(found) if found else None
        if aapt is None or not aapt.is_file():
            raise BundleError(
                "build-apks precisa de um aapt2 executável; use --aapt2 do apkfast "
                "ou instale aapt2 no PATH."
            )
        out_dir = Path(work).expanduser() if work is not None else bundle.parent
        out_dir.mkdir(parents=True, exist_ok=True)
        apks_path = out_dir / "verify.apks"
        args = [
            "build-apks",
            f"--bundle={bundle}",
            f"--output={apks_path}",
            "--mode=universal",
            f"--aapt2={aapt}",
        ]
        if keystore is not None:
            ks = Path(keystore).expanduser()
            if not ks.is_file():
                raise BundleError(f"Keystore não encontrado: {ks}")
            args += [
                f"--ks={ks}",
                f"--ks-key-alias={key_alias}",
                f"--ks-pass=pass:{store_password}",
                f"--key-pass=pass:{key_password}",
            ]
        run_bundletool(bundletool, args, quiet=quiet, java=java)
        try:
            with zipfile.ZipFile(apks_path) as archive:
                names = set(archive.namelist())
        except (OSError, zipfile.BadZipFile) as exc:
            raise BundleError(f"APK set gerado é inválido: {exc}") from exc
        if "universal.apk" not in names:
            raise BundleError(
                f"APK set não contém universal.apk (entradas: {', '.join(sorted(names))})"
            )
    return VerificationResult(
        manifest_xml=manifest,
        validate_output=validate.stdout or "",
        apks_path=apks_path,
    )
