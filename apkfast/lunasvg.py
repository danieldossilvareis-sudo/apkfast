"""Download/prepara o LunaSVG (SVG completo) como 2 TUs amalgamadas."""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path
from urllib.request import urlretrieve

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / "vendor" / "lunasvg"
SRC_CACHE = Path(os.environ.get("APKFAST_LUNASVG_SRC", str(VENDOR / "src")))

VERSION = "3.5.0+filters"
COMMIT = "470e78e2953dcf0d4b46322d64eed6ca84156fec"
TARBALL = f"lunasvg-{COMMIT[:12]}.tar.gz"
URL = f"https://github.com/sammycage/lunasvg/archive/{COMMIT}.tar.gz"
SHA256 = "4a4889417119d7740cf630093a273395190da30130477907e440269dd3cc928c"
FILTERS_PATCH = ROOT / "patches" / "lunasvg-filters.diff"


class LunaSVGError(RuntimeError):
    pass


def _say(message: str, quiet: bool = False) -> None:
    if not quiet:
        print(message, flush=True)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _has_sources(path: Path) -> bool:
    return (path / "source" / "lunasvg.cpp").is_file() and (
        path / "plutovg" / "source" / "plutovg-canvas.c"
    ).is_file()


def ensure_source(*, quiet: bool = False) -> Path:
    if _has_sources(SRC_CACHE):
        return SRC_CACHE
    SRC_CACHE.mkdir(parents=True, exist_ok=True)
    if any(SRC_CACHE.iterdir()):
        raise LunaSVGError(
            f"Fonte LunaSVG incompleta em {SRC_CACHE}. Remova o diretório "
            "ou ajuste APKFAST_LUNASVG_SRC."
        )

    archive = SRC_CACHE.parent / TARBALL
    if not archive.is_file() or _sha256(archive) != SHA256:
        _say(f"[*] Baixando LunaSVG {VERSION} ...", quiet)
        with tempfile.NamedTemporaryFile(
            prefix="lunasvg-", suffix=".tar.gz", delete=False
        ) as handle:
            tmp = Path(handle.name)
        try:
            urlretrieve(URL, tmp)
            actual = _sha256(tmp)
            if actual != SHA256:
                raise LunaSVGError(
                    f"SHA256 do LunaSVG inválido: {actual} (esperado {SHA256})"
                )
            shutil.move(str(tmp), archive)
        finally:
            tmp.unlink(missing_ok=True)

    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            name = member.name
            if name.startswith("/") or ".." in Path(name).parts:
                raise LunaSVGError(f"Membro inseguro no tarball LunaSVG: {name}")
        tar.extractall(SRC_CACHE.parent)
    extracted = SRC_CACHE.parent / f"lunasvg-{COMMIT}"
    if not _has_sources(extracted):
        raise LunaSVGError(f"Fonte LunaSVG inválida em {extracted}")

    if not FILTERS_PATCH.is_file():
        raise LunaSVGError(f"Patch de filtros não encontrado: {FILTERS_PATCH}")
    try:
        result = subprocess.run(
            ["git", "apply", str(FILTERS_PATCH)],
            cwd=extracted,
            capture_output=True,
            text=True,
        )
    except OSError as exc:
        raise LunaSVGError(f"git apply indisponível: {exc}") from exc
    if result.returncode != 0:
        raise LunaSVGError(
            "Falha ao aplicar patch de filtros SVG: " + (result.stderr or result.stdout)
        )

    if extracted != SRC_CACHE:
        extracted.rename(SRC_CACHE)
    return SRC_CACHE


def _amalgamate(source: Path) -> tuple[Path, Path]:
    unity = VENDOR / "unity"
    unity.mkdir(parents=True, exist_ok=True)

    c_out = unity / "plutovg_all.c"
    with c_out.open("w") as out:
        out.write("/* Gerado por apkfast a partir do LunaSVG " + VERSION + ". */\n")
        for path in sorted((source / "plutovg" / "source").glob("*.c")):
            out.write(f"\n/* ==== {path.name} ==== */\n")
            out.write(path.read_text(errors="ignore"))

    cpp_out = unity / "lunasvg_all.cpp"
    with cpp_out.open("w") as out:
        out.write("/* Gerado por apkfast a partir do LunaSVG " + VERSION + ". */\n")
        for path in sorted((source / "source").glob("*.cpp")):
            out.write(f"\n/* ==== {path.name} ==== */\n")
            out.write(path.read_text(errors="ignore"))
    return c_out, cpp_out


def install(*, quiet: bool = False) -> tuple[Path, Path]:
    source = ensure_source(quiet=quiet)
    c_out, cpp_out = _amalgamate(source)

    include_dir = VENDOR / "include"
    include_dir.mkdir(parents=True, exist_ok=True)
    for name in ("lunasvg.h",):
        candidate = source / "include" / name
        if candidate.is_file():
            shutil.copy2(candidate, include_dir / name)
    for name in ("LICENSE",):
        candidate = source / name
        if candidate.is_file():
            shutil.copy2(candidate, include_dir / ("LUNASVG-" + name))
    ftl = source / "plutovg" / "FTL.TXT"
    if ftl.is_file():
        shutil.copy2(ftl, include_dir / "PLUTOVG-FTL.TXT")

    _say(
        f"[✓] LunaSVG preparado: {c_out.name}, {cpp_out.name}",
        quiet,
    )
    return c_out, cpp_out
