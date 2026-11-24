"""The no-Gradle APK build pipeline."""

from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

from . import bundle as bundle_mod
from . import cache as cache_mod
from . import config_schema, lockfile as lock_mod
from . import resource_cache as res_cache
from . import signing as signing_mod
from .manifest import ManifestInfo, is_native_activity, parse_manifest
from .toolchain import NDK_ARCH_DIR, Toolchain, detect

DEFAULT_IGNORED_DIRS = {
    ".git", "build", "obj", ".apkfast", ".apkfast-cache", "gradle", ".gradle"
}
ALL_ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
CPP_SUFFIXES = (".cpp", ".cc", ".cxx", ".c++", ".C")
# zipalign antigos rejeitam -P com uma destas mensagens; qualquer outra falha
# deve abortar o build em vez de ser confundida com "sem suporte a 16 KB".
_ZIPALIGN_NO_P = re.compile(
    r"invalid option|invalid flag|unknown option|unknown flag|"
    r"unrecognized option|illegal option",
    re.IGNORECASE,
)


class BuildError(RuntimeError):
    pass


class Logger:
    """Controla verbosidade: quiet > normal > verbose."""

    def __init__(self, *, quiet: bool = False, verbose: bool = False) -> None:
        self.quiet = quiet
        self.verbose = verbose

    def info(self, message: str) -> None:
        if not self.quiet:
            print(message, flush=True)

    def command(self, cmd: list[str]) -> None:
        if self.verbose and not self.quiet:
            print(f"  $ {' '.join(str(c) for c in cmd)}", flush=True)

    def detail(self, message: str) -> None:
        if self.verbose and not self.quiet:
            print(message, flush=True)

    def warn(self, message: str) -> None:
        if not self.quiet:
            print(message, file=sys.stderr, flush=True)

    def error(self, message: str) -> None:
        # Erros sempre aparecem, mesmo em --quiet.
        print(message, file=sys.stderr, flush=True)


@dataclass
class BuildOptions:
    project: Path
    output: Path | None = None
    aab: bool = False
    aab_only: bool = False
    aab_output: Path | None = None
    bundletool_version: str = bundle_mod.BUNDLETOOL_VERSION
    built_aab: Path | None = None        # preenchido por build() quando --aab
    build_dir: Path | None = None
    abi: str = "arm64-v8a"          # usar "all" para fat APK
    abis: list[str] = field(default_factory=list)
    api: int = 36
    min_sdk: int = 21
    target_sdk: int = 36
    debug: bool = True
    release: bool = False
    optimize: str = "release"      # none | release | size
    obfuscate: bool = False
    libcxx_shared: bool = False
    vulkan: bool = False
    use_nativeapp: bool = True
    keystore: Path | None = None
    keystore_password: str | None = None  # None = APKFAST_STORE_PASS / "android"
    key_alias: str | None = None          # None = APKFAST_KEY_ALIAS / padrão
    key_password: str | None = None       # None = APKFAST_KEY_PASS / "android"
    v1_signing: bool | None = None
    v2_signing: bool | None = None
    v3_signing: bool | None = None
    v4_signing: bool | None = None
    version_code_strategy: str = "manifest+1"
    version_code: int | None = None
    timestamp_version_code: bool = False
    sdk_dir: str | None = None
    ndk_dir: str | None = None
    build_tools: str | None = None
    extra_cflags: list[str] = field(default_factory=list)
    extra_ldflags: list[str] = field(default_factory=list)
    sources: list[str] = field(default_factory=list)
    libraries: list[str] = field(default_factory=list)
    libs_dir: Path | None = None
    assets_dir: Path | None = None
    incremental: bool | None = None  # None = usar apkfast.json (default: true)
    clean_cache: bool = False
    cache_dir: Path | None = None
    cache_mode: str | None = None
    cache_namespace: str = ""
    require_cache_signature: bool | None = None
    cache_ctx: cache_mod.CacheContext | None = None  # resolvido em build()
    ignore_lock: bool = False                 # ignora divergência do lockfile
    lock_path: Path | None = None             # default: <projeto>/apkfast.lock.json
    verbose: bool = False
    quiet: bool = False
    log: Logger | None = None
    # Diagnóstico da v2.6.0 (preenchido durante build(), consumido pelo --report).
    cache_events: list[dict] = field(default_factory=list)
    timings_ms: dict[str, int] = field(default_factory=dict)
    cache_keystore_fingerprint: dict = field(default_factory=dict)
    cache_dex_key: str = ""      # reaproveitado por aab_key() no mesmo build
    cache_aab_key: str = ""

    def __post_init__(self) -> None:
        if self.log is None:
            self.log = Logger(quiet=self.quiet, verbose=self.verbose)

    def selected_abis(self) -> list[str]:
        if self.abis:
            return list(self.abis)
        if self.abi == "all":
            return list(ALL_ABIS)
        return [self.abi]


def _tail(text: str, limit: int = 40) -> str:
    """Limita o trecho de saída anexado a mensagens de erro em --quiet."""
    lines = text.rstrip().splitlines()
    if len(lines) <= limit:
        return "\n".join(lines)
    omitted = len(lines) - limit
    return f"... ({omitted} linha(s) omitida(s))\n" + "\n".join(lines[-limit:])


@contextmanager
def _timed(options: "BuildOptions", stage: str) -> Iterator[None]:
    """Acumula milissegundos por etapa (soma quando a etapa roda mais de uma vez)."""
    started = time.perf_counter()
    try:
        yield
    finally:
        elapsed = int(round((time.perf_counter() - started) * 1000))
        options.timings_ms[stage] = options.timings_ms.get(stage, 0) + elapsed


def _cache_event(
    options: "BuildOptions",
    stage: str,
    result: str,
    reason: str,
    key: str = "",
) -> None:
    """Registra hit/miss e, em -v, explica o miss com uma linha curta."""
    event = res_cache.cache_event(stage, result, reason, key)
    options.cache_events.append(event)
    log = options.log
    if log is not None and event["result"] == "miss":
        log.detail(f"[cache] {stage} miss: {event['reason']} ({event['key']})")


def _execute(
    cmd: list[str],
    log: Logger,
    *,
    cwd: Path | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess:
    """Executa um comando, transformando falhas de execução em BuildError."""
    log.command(signing_mod.redact_args(cmd))
    argv = [str(c) for c in cmd]
    try:
        return subprocess.run(
            argv,
            cwd=cwd,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
            text=capture,
        )
    except OSError as exc:
        raise BuildError(f"Não foi possível executar '{argv[0]}': {exc}") from exc


def _run(cmd: list[str], log: Logger, *, cwd: Path | None = None) -> subprocess.CompletedProcess:
    # Em --quiet a saída dos subprocessos precisa ser capturada: sem isso
    # avisos de clang/aapt2/keytool vazam mesmo com o logger silenciado.
    capture = log.quiet
    result = _execute(cmd, log, cwd=cwd, capture=capture)
    if result.returncode != 0:
        output = _tail(result.stdout or "") if capture else ""
        raise BuildError(
            f"Comando falhou ({result.returncode}): "
            + " ".join(signing_mod.redact_args(cmd))
            + (f"\n{output}" if output else "")
        )
    return result


def _clean(path: Path) -> None:
    if path.is_symlink():
        raise BuildError(f"Diretório de build não pode ser symlink: {path}")
    if path.exists():
        if not path.is_dir():
            raise BuildError(f"--build-dir aponta para um arquivo, não diretório: {path}")
        try:
            shutil.rmtree(path)
        except OSError as exc:
            raise BuildError(f"Falha ao limpar {path}: {exc}") from exc
    try:
        path.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise BuildError(f"Falha ao criar {path}: {exc}") from exc


def _api_headers_key(api_include: Path) -> str:
    """Hash de todos os headers/impls do nativeapp (invalidam a detecção)."""
    if not api_include.is_dir():
        return ""
    digest = hashlib.sha256()
    for path in sorted(api_include.rglob("*")):
        if path.suffix not in (".h", ".hpp", ".c", ".cpp"):
            continue
        digest.update(str(path.relative_to(api_include)).encode("utf-8", "replace"))
        digest.update(cache_mod.sha256_file(path).encode("ascii"))
    return digest.hexdigest()


def _strip_c_comments(text: str) -> str:
    """Apaga comentários e literais para detectar features sem falsos positivos.

    Mantém o tamanho aproximado do texto (comentários viram espaços) para que
    detecções como `#include <vulkan/vulkan.h>` continuem funcionando, enquanto
    `// #include <vulkan/vulkan.h>` e strings de log não ativam a feature.
    """
    out: list[str] = []
    state = "code"
    index = 0
    length = len(text)
    while index < length:
        char = text[index]
        following = text[index + 1] if index + 1 < length else ""
        if state == "code":
            if char == "/" and following == "/":
                state, index = "line", index + 2
                out.append("  ")
            elif char == "/" and following == "*":
                state, index = "block", index + 2
                out.append("  ")
            elif char in ('"', "'"):
                state = "string" if char == '"' else "char"
                out.append(" ")
                index += 1
            else:
                out.append(char)
                index += 1
        elif state == "line":
            if char == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            index += 1
        elif state == "block":
            if char == "*" and following == "/":
                state, index = "code", index + 2
                out.append("  ")
            else:
                out.append("\n" if char == "\n" else " ")
                index += 1
        else:  # string / char literal
            if char == "\\":
                out.append(" ")
                index += 2
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
                out.append(" ")
                index += 1
            else:
                out.append("\n" if char == "\n" else " ")
                index += 1
    return "".join(out)


def _read_sources(sources: list[Path]) -> str:
    """Concatena o código relevante (sem comentários) para detecção."""
    chunks: list[str] = []
    for source in sources:
        if not source.is_file():
            continue
        try:
            chunks.append(_strip_c_comments(source.read_text(errors="ignore")))
        except OSError as exc:
            raise BuildError(f"Não foi possível ler {source}: {exc}") from exc
    return "\n".join(chunks)


def _iter_files(root: Path, pattern: str, ignored: set[str]) -> list[Path]:
    found: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in ignored]
        for name in filenames:
            if name.endswith(pattern):
                found.append(Path(dirpath) / name)
    return sorted(found)


def _default_keystore() -> Path:
    return Path(os.environ.get("HOME", str(Path.home()))) / ".apkfast" / "debug.jks"


def ensure_debug_keystore(
    tc: Toolchain, path: Path, password: str, alias: str, log: Logger
) -> Path:
    if path.exists():
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    log.info(f"[*] Gerando keystore de debug: {path}")
    cmd = [
        tc.keytool,
        "-genkeypair",
        "-v",
        "-keystore",
        path,
        "-alias",
        alias,
        "-keyalg",
        "RSA",
        "-keysize",
        "2048",
        "-validity",
        "10950",  # 30 anos
        "-storepass",
        password,
        "-keypass",
        password,
        "-dname",
        "CN=apkfast debug, OU=dev, O=apkfast, L=Dev, ST=Dev, C=BR",
    ]
    _run(cmd, log)
    return path


def _compile_resources(
    tc: Toolchain,
    project: Path,
    staged_res: Path,
    work: Path,
    log: Logger,
    options: "BuildOptions | None" = None,
    manifest: ManifestInfo | None = None,
) -> list[Path]:
    res_dir = project / "res"
    if not res_dir.is_dir() or not any(res_dir.iterdir()):
        return []
    compiled_dir = work / "compiled_res"
    compiled_dir.mkdir(parents=True, exist_ok=True)

    ctx = options.cache_ctx if options is not None else None
    incremental = options is not None and options.incremental is not False
    key = ""
    if ctx is not None and manifest is not None:
        if incremental:
            key = res_cache.resource_key(project, manifest, options, tc, ctx)
            try:
                flats = res_cache.load_resource(ctx, key, compiled_dir)
                _cache_event(
                    options,
                    "resource",
                    "hit",
                    f"{len(flats)} flat(s) reaproveitado(s) do cache",
                    key,
                )
                return flats
            except (res_cache.CacheMiss, OSError) as exc:
                _cache_event(options, "resource", "miss", str(exc), key)
        else:
            _cache_event(options, "resource", "miss", "incremental desligado (--no-incremental)")

    # Preserve resource qualifiers in the flat filename: values-pt/strings.xml
    # becomes values-pt_strings.arsc.flat, which aapt2 link understands.
    flat_files: list[Path] = []
    for source in sorted(res_dir.rglob("*")):
        if not source.is_file():
            continue
        if source.name.startswith("."):
            continue
        relative = source.relative_to(res_dir)
        flat_name = "_".join(relative.parts) + ".flat"
        output = compiled_dir / flat_name
        _run([tc.aapt2, "compile", "-o", str(output), str(source)], log)
        # Algumas versões do aapt2 (inclusive a 2.20 usada no Termux) gravam
        # um ZIP com um único .flat dentro mesmo quando -o é um arquivo.
        # aapt2 link não aceita esse ZIP: extrai o .flat real antes de linkar.
        flat_files.extend(_unpack_compiled_flat(output, compiled_dir))
    flat_files = sorted(set(flat_files))
    if ctx is not None and key and incremental:
        res_cache.store_resource(ctx, key, flat_files)
    return flat_files


def _unpack_compiled_flat(output: Path, compiled_dir: Path) -> list[Path]:
    if not zipfile.is_zipfile(output):
        return [output]
    try:
        with zipfile.ZipFile(output) as archive:
            names = archive.namelist()
            if len(names) != 1:
                raise BuildError(
                    f"aapt2 compile gerou {len(names)} arquivos em {output} (esperado 1)."
                )
            name = names[0]
            if (
                not name.endswith(".flat")
                or "/" in name
                or "\\" in name
                or ".." in name
                or Path(name).is_absolute()
            ):
                raise BuildError(f"Nome de .flat inesperado no ZIP {output}: {name!r}")
            payload = archive.read(name)
    except (OSError, zipfile.BadZipFile) as exc:
        raise BuildError(f"Falha ao ler saída do aapt2 {output}: {exc}") from exc
    inner = compiled_dir / name
    output.unlink(missing_ok=True)
    inner.write_bytes(payload)
    return [inner]


def _collect_sources(project: Path, explicit: list[str]) -> list[Path]:
    if explicit:
        sources = []
        for item in explicit:
            path = Path(item)
            if not path.is_absolute():
                path = project / path
            if not path.is_file():
                raise BuildError(f"Arquivo de código não encontrado: {path}")
            sources.append(path)
        return sources
    sources = _iter_files(project, ".c", DEFAULT_IGNORED_DIRS)
    sources += _iter_files(project, ".cpp", DEFAULT_IGNORED_DIRS)
    sources += _iter_files(project, ".cc", DEFAULT_IGNORED_DIRS)
    return sorted(set(sources))


def _collect_project_libs(
    project: Path,
    tc: Toolchain,
    options: "BuildOptions",
    log: Logger,
) -> tuple[list[Path], list[Path]]:
    """Coleta .a (link) e .so (embed) do projeto para a ABI atual.

    Convenção: <projeto>/libs/<abi>/*.a e *.so. Também aceita --lib e
    --libs-dir. Nunca cruza ABIs: um .a de arm64 nunca é linkado em x86.
    """
    abi = tc.abi
    static_libs: list[Path] = []
    shared_libs: list[Path] = []

    roots: list[Path] = []
    default_libs = project / "libs"
    if options.libs_dir is not None:
        custom = options.libs_dir.expanduser()
        if not custom.is_absolute():
            custom = project / custom
        roots.append(custom.resolve())
    elif default_libs.is_dir():
        roots.append(default_libs)

    for raw in options.libraries:
        path = Path(raw).expanduser()
        if not path.is_absolute():
            path = project / path
        path = path.resolve()
        if not path.exists():
            raise BuildError(f"Biblioteca não encontrada: {path}")
        if path.is_dir():
            roots.append(path)
        else:
            _classify_lib(path, abi, static_libs, shared_libs, explicit=True)

    for root in roots:
        abi_dir = root / abi
        candidates = sorted(abi_dir.glob("*.a")) + sorted(abi_dir.glob("*.so")) if abi_dir.is_dir() else []
        # Compatibilidade: libs/ soltas (sem subdir de ABI) só valem para
        # builds de uma ABI única; em fat APK isso é ambíguo e falha.
        if not candidates and root.is_dir():
            loose = sorted(root.glob("*.a")) + sorted(root.glob("*.so"))
            if loose and len(options.selected_abis()) == 1:
                candidates = loose
            elif loose:
                raise BuildError(
                    f"Bibliotecas soltas em {root} são ambíguas em fat APK. "
                    f"Use libs/<abi>/ ex.: libs/{abi}/"
                )
        for path in candidates:
            _classify_lib(path, abi, static_libs, shared_libs, explicit=False)

    # Dedup preservando ordem (importante para resolução de símbolos).
    static_libs = list(dict.fromkeys(static_libs))
    shared_libs = list(dict.fromkeys(shared_libs))
    if static_libs or shared_libs:
        log.info(
            f"[*] Bibliotecas do projeto ({abi}): "
            f"{len(static_libs)} estática(s), {len(shared_libs)} compartilhada(s)"
        )
    return static_libs, shared_libs


def _classify_lib(
    path: Path,
    abi: str,
    static_libs: list[Path],
    shared_libs: list[Path],
    *,
    explicit: bool,
) -> None:
    if not path.is_file():
        return
    if path.parent.name in ALL_ABIS and path.parent.name != abi:
        return  # biblioteca de outra ABI: ignora
    if path.suffix == ".a":
        static_libs.append(path)
    elif path.suffix == ".so":
        shared_libs.append(path)
    elif explicit:
        raise BuildError(
            f"Biblioteca não suportada: {path} (use .a ou .so)"
        )


def _native_build(
    tc: Toolchain,
    options: BuildOptions,
    project: Path,
    work: Path,
    manifest: ManifestInfo,
) -> tuple[Path | None, list[Path]]:
    """Compila a biblioteca nativa.

    Retorna (lib .so, libs compartilhadas adicionais para incluir no APK).
    """
    log = options.log
    assert log is not None
    sources = _collect_sources(project, options.sources)
    if not sources:
        return None, []

    project_statics, project_shareds = _collect_project_libs(project, tc, options, log)

    # Injeta o TU com as implementações third-party do nativeapp (stb_image,
    # stb_truetype, stb_vorbis, dr_mp3) exatamente uma vez por build.
    if options.use_nativeapp:
        # Detecta raylib antes da injeção (ela já define os símbolos stbi_*).
        uses_raylib_here = "raylib.h" in _read_sources(sources)
        impl_name = (
            "nativeapp_impl_no_image.cpp" if uses_raylib_here else "nativeapp_impl.cpp"
        )
        impl = Path(__file__).resolve().parent.parent / "src" / impl_name
        if impl.is_file():
            injected = work / "injected" / impl_name
            injected.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(impl, injected)
            sources.append(injected)
            log.detail(f"[*] TU third-party do nativeapp injetado ({impl_name}).")

    # LunaSVG: 2 TUs amalgamadas (C + C++) quando preparadas por lunasvg-install.
    luna_dir = Path(__file__).resolve().parent.parent / "vendor" / "lunasvg"
    luna_c = luna_dir / "unity" / "plutovg_all.c"
    luna_cpp = luna_dir / "unity" / "lunasvg_all.cpp"
    use_svg_full = (
        options.use_nativeapp and luna_c.is_file() and luna_cpp.is_file()
    )
    if use_svg_full:
        sources += [luna_c, luna_cpp]
        log.detail("[*] LunaSVG injetado (SVG com CSS/texto/clip/mask/pattern).")

    native_dir = work / "native" / NDK_ARCH_DIR[tc.abi]
    native_dir.mkdir(parents=True, exist_ok=True)
    objects: list[Path] = []

    ctx = options.cache_ctx
    if ctx is None:
        raise BuildError("Contexto de cache não foi resolvido")
    abi = tc.abi
    root = Path(__file__).resolve().parent.parent
    api_include = root / "include"
    roots = cache_mod.make_roots(
        project=project, apkfast_root=root, tc=tc, work=work, cache_root=ctx.root
    )
    api_key = _api_headers_key(api_include) if options.use_nativeapp else ""
    cache_data = cache_mod.read_cache(ctx, abi)

    def save_cache() -> None:
        cache_mod.write_cache(ctx, abi, cache_data)

    compiler_fps: dict[str, str] = {}
    object_hits = 0
    object_misses = 0

    def compiler_fp(compiler: Path) -> str:
        key = str(compiler)
        if key not in compiler_fps:
            glue_source = tc.native_app_glue_dir() / "android_native_app_glue.c"
            compiler_fps[key] = cache_mod.compiler_fingerprint(
                compiler,
                abi=abi,
                api=tc.api,
                target_flags=tc.clang_target_flags(
                    explicit_target=compiler.name.startswith("clang")
                ),
                glue_source=glue_source,
                roots=roots,
            )
        return compiler_fps[key]

    def compile_unit(
        source: Path,
        *,
        compiler: Path,
        flags: list[str],
        extra_key: str = "",
    ) -> Path:
        """Compila uma TU com chave portável por fonte/flags/headers."""
        nonlocal object_hits, object_misses
        source_hash = cache_mod.sha256_file(source)
        key = cache_mod.object_key(
            source=source,
            flags=flags,
            compiler_fp=compiler_fp(compiler),
            api_key=api_key,
            abi=abi,
            api=tc.api,
            project_key=ctx.project_id,
            roots=roots,
            extra_key=extra_key,
        )
        dep = ctx.depfile(abi, key)
        dep.parent.mkdir(parents=True, exist_ok=True)
        entry = cache_data["objects"].get(key)
        reused = False
        obj_path: Path | None = None
        if options.incremental is not False and isinstance(entry, dict):
            try:
                obj_path = cache_mod.ensure_blob(
                    ctx, entry.get("obj_blob"), abi=abi, check_elf=False
                )
                state = cache_mod.verify_dep_state(entry.get("deps_state", []), roots)
                digest_state = cache_mod.sha256_text(
                    json.dumps(state, sort_keys=True, separators=(",", ":"))
                )
                reused = (
                    entry.get("src_hash") == source_hash
                    and entry.get("deps_digest") == digest_state
                )
            except (cache_mod.CacheError, OSError):
                reused = False
                obj_path = None
        if reused and obj_path is not None:
            object_hits += 1
            log.detail(f"[cache] reuso: {source.name}")
            return obj_path
        object_misses += 1

        obj_tmp = ctx.abi_dir(abi) / f"obj_{key[:20]}.o"
        cmd = [compiler, "-c", str(source), "-o", str(obj_tmp),
               "-MD", "-MF", str(dep), *flags]
        _run(cmd, log)
        obj_path = ctx.blob_path(cache_mod.write_blob(ctx, obj_tmp.read_bytes()))
        obj_tmp.unlink(missing_ok=True)

        deps = cache_mod.parse_depfile(dep)
        state = cache_mod.dep_state(deps, roots)
        cache_data["objects"][key] = {
            "src": cache_mod.logical_path(source, roots),
            "src_hash": source_hash,
            "deps": deps,
            "deps_state": state,
            "deps_digest": cache_mod.sha256_text(
                json.dumps(state, sort_keys=True, separators=(",", ":"))
            ),
            "obj_blob": obj_path.name,
            "abi": abi,
        }
        save_cache()
        return obj_path

    optimization = {
        "none": "-O0 -g",
        "release": "-O2 -DNDEBUG",
        "size": "-Os -DNDEBUG",
    }.get(options.optimize, "-O2 -DNDEBUG")

    base_flags = [
        "-fPIC",
        "-ffunction-sections",
        "-fdata-sections",
        "-fstack-protector-strong",
    ]
    source_text = _read_sources(sources)
    uses_vulkan = options.vulkan or any(
        token in source_text
        for token in ("nativeapp/vk.hpp", "nativeapp/nativeapp.hpp",
                      "vulkan/vulkan.h", "VulkanScene")
    )
    uses_gles = (
        "GLES2Scene" in source_text
        or "GLES/egl.h" in source_text
        or "GLES2/gl2.h" in source_text
        or "GLES3/gl3.h" in source_text
    )
    uses_raylib = "raylib.h" in source_text
    uses_sdl3 = "SDL3/SDL.h" in source_text
    uses_sdl2 = not uses_sdl3 and "SDL.h" in source_text

    # LTO com Android/Vulkan (NDK r30) remove o DT_NEEDED de libvulkan.so e
    # deixa os símbolos vk* sem resolução -> dlopen falha. Desativamos LTO
    # quando Vulkan for usado.
    use_lto = options.optimize != "none" and not uses_vulkan
    if use_lto:
        base_flags.append("-flto")

    # Ofuscação nativa: esconde símbolos internos e remove tabelas de símbolos.
    if options.obfuscate:
        base_flags.append("-fvisibility=hidden")
        base_flags.append("-fvisibility-inlines-hidden")

    def compiler_flags(compiler: Path) -> list[str]:
        # Wrappers versionados (ex.: aarch64-linux-android36-clang) já embutem
        # o target. clang-18 e outros nomes genéricos precisam de --target.
        return tc.clang_target_flags(explicit_target=compiler.name.startswith("clang"))

    # Diretório da API nativeapp (headers ficam em <raiz do apkfast>/include).
    raylib_dir = root / "vendor" / "raylib"
    raylib_static = raylib_dir / "lib" / f"libraylib-{tc.abi}.a"
    webp_dir = root / "vendor" / "webp"
    webp_inc = webp_dir / "include"
    webp_demux = webp_dir / "lib" / f"libwebpdemux-{tc.abi}.a"
    webp_decoder = webp_dir / "lib" / f"libwebpdecoder-{tc.abi}.a"
    use_webp_full = (
        options.use_nativeapp and webp_demux.is_file() and webp_decoder.is_file()
    )

    common_flags = [
        *optimization.split(),
        "-Wall",
        f"-I{project}",
        f"-I{project / 'jni'}",
        f"-I{project / 'src' / 'main' / 'cpp'}",
        f"-I{tc.native_app_glue_dir()}",
        *( [f"-I{api_include}"] if options.use_nativeapp and api_include.is_dir() else [] ),
        *( [f"-I{webp_inc}", "-DAPKFAST_WEBP_FULL=1"] if use_webp_full else [] ),
        *(
            [
                "-DAPKFAST_SVG_FULL=1",
                "-DLUNASVG_BUILD", "-DLUNASVG_BUILD_STATIC",
                "-DLUNASVG_DISABLE_LOAD_SYSTEM_FONTS",
                "-DLUNASVG_DISABLE_EXTERNAL_RESOURCES",
                "-DPLUTOVG_BUILD", "-DPLUTOVG_BUILD_STATIC",
                "-DPLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD",
                f"-I{luna_dir / 'include'}",
                f"-I{luna_dir / 'src' / 'include'}",
                f"-I{luna_dir / 'src' / 'source'}",
                f"-I{luna_dir / 'src' / 'plutovg' / 'include'}",
                f"-I{luna_dir / 'src' / 'plutovg' / 'source'}",
            ] if use_svg_full else []
        ),
        *( [f"-I{raylib_dir / 'include'}"] if uses_raylib and raylib_dir.is_dir() else [] ),
        *( [f"-I{root / 'vendor' / 'sdl2' / 'include'}"]
           if uses_sdl2 and (root / 'vendor' / 'sdl2' / 'include').is_dir() else [] ),
        *( [f"-I{root / 'vendor' / 'sdl3' / 'include'}"]
           if uses_sdl3 and (root / 'vendor' / 'sdl3' / 'include').is_dir() else [] ),
        # Caminhos determinísticos em __FILE__/debug info: essencial para um
        # objeto compilado numa máquina ser reaproveitado noutra.
        f"-ffile-prefix-map={project}=<project>",
        f"-ffile-prefix-map={root}=<apkfast>",
        f"-ffile-prefix-map={tc.ndk_dir}=<ndk>",
        "-fdebug-compilation-dir=<project>",
        *options.extra_cflags,
    ]

    log.info(f"[*] Compilando {len(sources)} arquivo(s) nativo(s) para {tc.abi}")
    for source in sources:
        is_cpp = source.suffix in CPP_SUFFIXES
        compiler = tc.clang(cpp=is_cpp)
        flags = (
            compiler_flags(compiler)
            + base_flags
            + (["-std=c++17"] if is_cpp else ["-std=c11"])
            + common_flags
        )
        objects.append(compile_unit(source, compiler=compiler, flags=flags))

    glue_source = tc.native_app_glue_dir() / "android_native_app_glue.c"
    glue_compiled = False
    # O raylib fornece android_main() (que chama main()), mas não fornece
    # ANativeActivity_onCreate — esse símbolo vem do glue do NDK. Portanto
    # compilamos o glue normalmente; o usuário deve definir main(), não
    # android_main(), em projetos raylib.
    # SDL3 usa SDLActivity (Java), que já fornece ANativeActivity_onCreate e
    # chama android_main() diretamente. Compilar o glue do NDK aqui causaria
    # referência dupla a android_main.
    if glue_source.is_file() and not uses_sdl3 and not uses_sdl2:
        glue_compiler = tc.clang()
        flags = compiler_flags(glue_compiler) + base_flags + ["-std=c11"] + common_flags
        objects.append(
            compile_unit(glue_source, compiler=glue_compiler, flags=flags, extra_key="glue")
        )
        glue_compiled = True

    if is_native_activity(manifest) and not uses_raylib and not uses_sdl3 and not uses_sdl2 and not glue_compiled:
        log.warn("[!] Aviso: NativeActivity declarada mas native_app_glue não foi compilado.")

    _cache_event(
        options,
        "native",
        "hit" if object_misses == 0 and object_hits else "miss",
        f"objetos: {object_hits} reaproveitado(s), {object_misses} compilado(s) ({abi})",
        ctx.project_id,
    )

    lib_name = _native_lib_name(manifest, project)
    output = native_dir / f"lib{lib_name}.so"
    so_tmp = ctx.abi_dir(abi) / f"lib{lib_name}.link.so"
    has_cpp = any(s.suffix in CPP_SUFFIXES for s in sources)
    linker = tc.clang(cpp=has_cpp)
    cmd = [linker, "-shared", "-o", str(so_tmp)]
    cmd += compiler_flags(linker)
    cmd += [
        "-Wl,--gc-sections",
        "-Wl,-z,relro",
        "-Wl,-z,now",
    ]
    if options.obfuscate:
        # Remove símbolos de bibliotecas estáticas e marca seções como não
        # exportadas, dificultando engenharia reversa.
        cmd.append("-Wl,--exclude-libs,ALL")
        cmd.append("-Wl,-z,noexecstack")
    # Bibliotecas do sistema Android. -llog é essencial: sem ele o dlopen
    # falha com "cannot locate symbol __android_log_print".
    cmd += [
        *objects,
        # Bibliotecas estáticas do projeto: antes das libs do sistema para
        # que os símbolos sejam resolvidos na ordem correta.
        *[str(lib) for lib in project_statics],
        # libwebp (WebP animado): demux depende do decoder, nessa ordem.
        *([str(webp_demux), str(webp_decoder)] if use_webp_full else []),
        "-lm",
        "-ldl",
        "-landroid",
        "-llog",
        # Stub do AndroidBitmap_* usado pelo decode WebP via BitmapFactory.
        *( ["-ljnigraphics"] if options.use_nativeapp else [] ),
        "-Wl,--no-as-needed", "-lOpenSLES",
        *( ["-laaudio"] if tc.api >= 26 else [] ),
        "-Wl,--as-needed",
        *( ["-lEGL", "-lGLESv2"] if uses_gles else [] ),
        *options.extra_ldflags,
    ]
    extra_libs: list[Path] = []
    extra_libs.extend(project_shareds)
    if has_cpp:
        # Padrão: libc++ estático (APK menor, sem dependência externa).
        # --libcxx-shared usa libc++_shared.so e a embute no APK.
        if options.libcxx_shared and not options.obfuscate:
            cmd.append("-lc++_shared")
            shared = tc.ndk_shared_lib(tc.abi, "libc++_shared.so")
            if shared:
                extra_libs.append(shared)
        else:
            cmd.append("-static-libstdc++")

    if uses_sdl2 or uses_sdl3:
        sdl_num = 3 if uses_sdl3 else 2
        sdl_dir = root / "vendor" / f"sdl{sdl_num}"
        sdl_static = sdl_dir / "lib" / f"libSDL{sdl_num}-{tc.abi}.a"
        if not sdl_static.is_file():
            raise BuildError(
                f"SDL{sdl_num} não encontrada para {tc.abi}: {sdl_static}. "
                f"Rode: python3 apkfast.py sdl{sdl_num}-install --abi {tc.abi}"
            )
        cmd.append(str(sdl_static))
        cmd += [
            "-lGLESv1_CM", "-lGLESv2", "-lOpenSLES", "-landroid",
            "-llog", "-ldl", "-lz", "-lm",
        ]

    if uses_raylib:
        if not raylib_static.is_file():
            raise BuildError(
                f"raylib não encontrada para {tc.abi}: {raylib_static}. "
                "Rode: python3 apkfast.py raylib-install"
            )
        # Ordem importa: libraylib.a primeiro, depois as dependências.
        cmd.append(str(raylib_static))
        cmd += [
            "-Wl,--wrap=fopen",  # raylib usa __real_fopen/__wrap_fopen no Android
            "-lGLESv2", "-lEGL", "-landroid", "-llog", "-lz", "-lm",
        ]

    if uses_vulkan:
        # -lvulkan usa o stub do NDK apenas para linkar. NÃO embutir esse
        # stub no APK: em runtime o Android fornece o libvulkan.so real do
        # dispositivo (via nativeloader).
        #
        # O NDK r30 + LTO aplica --as-needed e remove o DT_NEEDED do
        # libvulkan.so, deixando os vk* como UND sem biblioteca -> dlopen
        # falha com "cannot locate symbol vkCreateInstance". Forçamos o
        # DT_NEEDED com --no-as-needed.
        cmd += ["-Wl,--no-as-needed", "-lvulkan", "-Wl,--as-needed"]
    # Link cacheado com chave portável e blob verificado.
    strip = tc.ndk_toolchain_bin / "llvm-strip"
    strip_mode = ""
    if (options.optimize != "none" or options.obfuscate) and strip.is_file():
        strip_mode = "--strip-all" if options.obfuscate else "--strip-unneeded"

    link_files = list(objects) + [
        Path(part) for part in cmd
        if part != str(so_tmp)
        and Path(part).is_file()
        and Path(part).suffix in (".a", ".so")
    ] + list(extra_libs)
    key = cache_mod.link_key(
        command=cmd,
        inputs=link_files,
        compiler_fp=compiler_fp(linker),
        strip_mode=strip_mode,
        lib_name=lib_name,
        abi=abi,
        project_key=ctx.project_id,
        roots=roots,
    )
    link_entry = cache_data.get("link")
    cached_so: Path | None = None
    reuse_link = False
    if options.incremental is not False and isinstance(link_entry, dict):
        try:
            if link_entry.get("key") == key:
                cached_so = cache_mod.ensure_blob(ctx, link_entry.get("so_blob"), abi=abi)
                reuse_link = True
        except (cache_mod.CacheError, OSError):
            cached_so = None
    if reuse_link and cached_so is not None:
        log.detail(f"[cache] reuso: lib{lib_name}.so")
        _cache_event(options, "native", "hit", f"link reaproveitado ({abi})", key)
    else:
        previous = link_entry.get("key") if isinstance(link_entry, dict) else None
        reason = (
            f"link: entrada ausente ({abi})"
            if previous is None
            else f"link: chave mudou ({abi}, salvo={str(previous)[:8]})"
        )
        log.detail(f"[cache] link miss: salvo={previous} novo={key}")
        _cache_event(options, "native", "miss", reason, key)
        _run(cmd, log)
        if strip_mode:
            _run([strip, strip_mode, str(so_tmp)], log)
        cached_so = ctx.blob_path(cache_mod.write_blob(ctx, so_tmp.read_bytes()))
        so_tmp.unlink(missing_ok=True)
        cache_data["link"] = {
            "key": key,
            "lib": lib_name,
            "so_blob": cached_so.name,
            "abi": abi,
        }
        save_cache()

    if cached_so.resolve() != output.resolve():
        shutil.copy2(cached_so, output)

    return output, extra_libs


def _native_lib_name(manifest: ManifestInfo, project: Path) -> str:
    """Derive the library name Android will look for.

    Uses android:hasCode=false convention: Android loads libmain.so for
    NativeActivity when android.app.NativeActivity is used directly. Projects
    can override by creating a jni/Application.mk with APP_MODULES, but for the
    common case we use 'main'.
    """
    for activity in manifest.activities:
        attrs = activity.attributes
        value = attrs.get("android:libraryName") or attrs.get("libraryName")
        if value:
            return value
    data = _read_project_config(project)
    name = data.get("nativeLibraryName")
    if name is not None:
        if not isinstance(name, str) or not name.strip():
            raise BuildError(
                f"nativeLibraryName em {project / 'apkfast.json'} deve ser um texto não vazio."
            )
        return name.strip()
    return "main"


def _read_project_config(project: Path) -> dict:
    """Lê apkfast.json com validação de chaves/tipos (config vazia = {})."""
    config = project / config_schema.CONFIG_NAME
    if not config.is_file():
        return {}
    import json

    try:
        data = json.loads(config.read_text())
    except (OSError, UnicodeError, ValueError) as exc:
        raise BuildError(f"apkfast.json inválido em {config}: {exc}") from exc
    try:
        return config_schema.validate(data, path=config)
    except config_schema.ConfigError as exc:
        raise BuildError(str(exc)) from exc


def _stage_native_lib(
    work: Path, abi: str, lib: Path | None, extra_libs: list[Path]
) -> Path | None:
    if lib is None:
        return None
    target = work / "apk" / "lib" / abi
    target.mkdir(parents=True, exist_ok=True)
    if not lib.is_file():
        raise BuildError(f"Biblioteca nativa não encontrada: {lib}")
    names = {lib.name}
    for extra in extra_libs:
        if not extra.is_file():
            raise BuildError(f"Biblioteca compartilhada não encontrada: {extra}")
        if extra.name in names:
            raise BuildError(
                f"Biblioteca duplicada para {abi}: {extra.name} "
                "(verifique libc++_shared.so e o nome da lib principal)."
            )
        names.add(extra.name)
    shutil.copy2(lib, target / lib.name)
    for extra in extra_libs:
        shutil.copy2(extra, target / extra.name)
    return target


def _find_tool(tc: Toolchain, name: str) -> Path | None:
    """Procura uma ferramenta no SDK (cmdline-tools/bin, build-tools) ou PATH."""
    candidates = [
        tc.sdk_dir / "cmdline-tools" / "bin" / name,
        tc.build_tools_dir / name,
        tc.build_tools_dir / "lib" / name,
    ]
    for path in candidates:
        if path.is_file() and os.access(path, os.X_OK):
            return path
    found = shutil.which(name)
    return Path(found) if found else None


def _compile_java(
    tc: Toolchain,
    options: BuildOptions,
    work: Path,
    log: Logger,
    manifest: ManifestInfo | None = None,
) -> Path | None:
    """Compila .java para classes e depois usa R8/D8 quando presente.

    Com incremental ativo, um hit do cache devolve ``classes.dex`` e pula
    ``javac`` e D8/R8 por completo. Retorna None se não houver código Java.
    """
    java_sources = _iter_files(options.project, ".java", DEFAULT_IGNORED_DIRS)
    jars = _iter_files(options.project, ".jar", DEFAULT_IGNORED_DIRS)

    if not java_sources and not jars:
        return None

    dex = work / "apk" / "classes.dex"
    dex.parent.mkdir(parents=True, exist_ok=True)
    javac = _find_tool(tc, "javac") or Path("javac")
    r8 = _find_tool(tc, "r8")
    d8 = _find_tool(tc, "d8") or tc.d8

    ctx = options.cache_ctx
    incremental = (
        options.incremental is not False
        and ctx is not None
        and manifest is not None
    )
    key = ""
    if incremental:
        assert ctx is not None and manifest is not None
        key = res_cache.dex_key(
            options.project, manifest, options, tc, ctx,
            d8=d8, r8=r8, javac=javac,
        )
        options.cache_dex_key = key
        try:
            restored = res_cache.load_dex(ctx, key, dex)
            _cache_event(
                options, "dex", "hit",
                "DEX reaproveitado do cache (javac/D8/R8 pulados)", key,
            )
            return restored
        except (res_cache.CacheMiss, OSError) as exc:
            _cache_event(options, "dex", "miss", str(exc), key)
    else:
        reason = (
            "incremental desligado (--no-incremental)"
            if options.incremental is False
            else "sem manifesto/cache para DEX"
        )
        _cache_event(options, "dex", "miss", reason)

    classes = work / "java_classes"
    classes.mkdir(parents=True, exist_ok=True)
    if java_sources:
        cmd = [
            javac,
            "-source", "17",
            "-target", "17",
            "-classpath", str(tc.platform_dir / "android.jar"),
            "-d", str(classes),
            *[str(s) for s in java_sources],
        ]
        _run(cmd, log)

    class_files = sorted(classes.rglob("*.class"))
    inputs = [str(c) for c in class_files] + [str(j) for j in jars]
    if not inputs:
        log.warn("[!] Nenhum .class/.jar para dexar.")
        return None

    if options.obfuscate and r8:
        log.info("[*] Ofuscando código Java com R8...")
        rules = options.project / "proguard-rules.pro"
        cmd = [
            r8,
            "--release",
            "--min-api", str(options.min_sdk),
            "--lib", str(tc.platform_dir / "android.jar"),
            "--output", str(dex),
            *inputs,
        ]
        if rules.is_file():
            cmd += ["--pg-conf", str(rules)]
        _run(cmd, log)
    elif d8:
        log.info("[*] Compilando código Java com D8...")
        cmd = [
            d8,
            "--min-api", str(options.min_sdk),
            "--lib", str(tc.platform_dir / "android.jar"),
            "--output", str(work / "apk"),
            *inputs,
        ]
        _run(cmd, log)
    else:
        log.warn("[!] Aviso: javac/D8/R8 não disponível; código Java ignorado.")
        return None
    if key and ctx is not None:
        res_cache.store_dex(ctx, key, dex)
    return dex


def _link_apk(
    tc: Toolchain,
    options: BuildOptions,
    work: Path,
    manifest_path: Path,
    manifest: ManifestInfo,
    flat_files: list[Path],
    dex: Path | None,
) -> Path:
    log = options.log
    assert log is not None
    apk = work / "apk" / "unsigned_unaligned.apk"
    cmd = [
        tc.aapt2,
        "link",
        "-o",
        str(apk),
        "--manifest",
        str(manifest_path),
        "-I",
        str(tc.platform_dir / "android.jar"),
        "--min-sdk-version",
        str(options.min_sdk),
        "--target-sdk-version",
        str(options.target_sdk),
        "--version-code",
        manifest.version_code or "1",
        "--version-name",
        manifest.version_name or "1.0",
        "--auto-add-overlay",
    ]
    for flat in flat_files:
        cmd += ["-R", str(flat)]
    assets = options.assets_dir or (options.project / "assets")
    if assets.is_dir():
        cmd += ["-A", str(assets)]
    _run(cmd, log)
    return apk


def _add_extra_files_with_zip(apk: Path, extra_root: Path) -> None:
    """Insere arquivos extras (lib/<abi>/*.so, classes.dex) no APK via zipfile.

    Evita depender de um binário `zip` externo e preserva manifesto e recursos
    gerados pelo aapt2.
    """
    extra_files = sorted(extra_root.rglob("*.so")) + sorted(extra_root.rglob("*.dex"))
    if not extra_files:
        return
    staged: list[tuple[Path, str]] = []
    seen: set[str] = set()
    for extra in extra_files:
        if not extra.is_file():
            raise BuildError(f"Arquivo para empacotar não encontrado: {extra}")
        arcname = str(extra.relative_to(extra_root)).replace(os.sep, "/")
        if arcname in seen:
            raise BuildError(f"Arquivo duplicado no APK: {arcname}")
        seen.add(arcname)
        staged.append((extra, arcname))

    tmp = apk.with_suffix(".withextra.apk")
    try:
        with zipfile.ZipFile(apk, "r") as src:
            existing = {info.filename for info in src.infolist()}
            for _, arcname in staged:
                if arcname in existing:
                    raise BuildError(
                        f"Conflito no APK: {arcname} já foi gerado pelo aapt2."
                    )
            with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED) as dst:
                for info in src.infolist():
                    dst.writestr(info, src.read(info.filename))
                for extra, arcname in staged:
                    # dex e .so precisam ser STORED: dex é mapeado pela ART e .so
                    # precisa ser page-aligned/comprimido pelo instalador em Android
                    # 15+ (16 KB pages). Comprimidos, o dlopen falha.
                    method = (
                        zipfile.ZIP_STORED
                        if extra.suffix in (".dex", ".so")
                        else zipfile.ZIP_DEFLATED
                    )
                    dst.write(extra, arcname, compress_type=method)
        tmp.replace(apk)
    except (OSError, zipfile.BadZipFile) as exc:
        raise BuildError(f"Falha ao empacotar arquivos extras no APK: {exc}") from exc


def _resolve_keystore(tc: Toolchain, options: BuildOptions, log: Logger) -> Path:
    """Keystore único para APK e AAB (debug criado sob demanda)."""
    keystore = options.keystore
    if keystore is None:
        if options.release:
            raise BuildError(
                "Modo release exige --keystore. Use --keystore outro.jks "
                "(ou rode sem --release para usar o keystore debug)."
            )
        keystore = _default_keystore()
        ensure_debug_keystore(
            tc, keystore, options.keystore_password, options.key_alias, log
        )
    elif not keystore.is_file():
        raise BuildError(f"Keystore não encontrado: {keystore}")
    return keystore


def _link_proto(
    tc: Toolchain,
    options: BuildOptions,
    work: Path,
    manifest_path: Path,
    manifest: ManifestInfo,
    flat_files: list[Path],
) -> Path:
    """Segundo link do aapt2, em protobuf, usado apenas pelo AAB."""
    log = options.log
    assert log is not None
    proto = work / "aab" / "proto.apk"
    proto.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        tc.aapt2,
        "link",
        "--proto-format",
        "-o",
        str(proto),
        "--manifest",
        str(manifest_path),
        "-I",
        str(tc.platform_dir / "android.jar"),
        "--min-sdk-version",
        str(options.min_sdk),
        "--target-sdk-version",
        str(options.target_sdk),
        "--version-code",
        manifest.version_code or "1",
        "--version-name",
        manifest.version_name or "1.0",
        "--auto-add-overlay",
    ]
    for flat in flat_files:
        cmd += ["-R", str(flat)]
    try:
        _run(cmd, log)
    except BuildError as exc:
        raise BuildError(
            f"{exc}\n[!] O aapt2 precisa aceitar --proto-format para gerar AAB. "
            "Use um aapt2 recente do Maven com.android.tools.build:aapt2."
        ) from exc
    return proto


def _align_and_sign(
    tc: Toolchain,
    options: BuildOptions,
    work: Path,
    unsigned: Path,
) -> Path:
    log = options.log
    assert log is not None
    aligned = work / "apk" / "unsigned_aligned.apk"
    # -P 16 alinha para páginas de 16 KB (Android 15+). Versões antigas do
    # zipalign aceitam -P; se não aceitar, tenta sem.
    align_cmd = [tc.zipalign, "-f", "-P", "16", "4", str(unsigned), str(aligned)]
    attempt = _execute(align_cmd, log, capture=True)
    if attempt.stdout and not log.quiet:
        log.info(attempt.stdout.rstrip())
    if attempt.returncode != 0:
        output = attempt.stdout or ""
        if not _ZIPALIGN_NO_P.search(output):
            raise BuildError(
                f"zipalign falhou ({attempt.returncode}): "
                f"{' '.join(str(c) for c in align_cmd)}"
                + (f"\n{_tail(output)}" if output else "")
            )
        log.warn("[!] zipalign sem suporte a -P 16; usando alinhamento 4.")
        _run([tc.zipalign, "-f", "4", str(unsigned), str(aligned)], log)

    keystore = _resolve_keystore(tc, options, log)

    signed = work / "apk" / "signed.apk"
    try:
        cmd = signing_mod.apksigner_sign_args(
            apksigner=tc.apksigner,
            keystore=keystore,
            store_pass=options.keystore_password or "android",
            key_pass=options.key_password or "android",
            key_alias=options.key_alias or "androiddebugkey",
            unsigned=aligned,
            output=signed,
            v1=options.v1_signing if options.v1_signing is not None else options.min_sdk < 24,
            v2=options.v2_signing if options.v2_signing is not None else True,
            v3=options.v3_signing if options.v3_signing is not None else True,
            v4=options.v4_signing if options.v4_signing is not None else True,
            warn=lambda message: log.warn(message),
        )
    except signing_mod.SigningError as exc:
        raise BuildError(str(exc)) from exc
    _run(cmd, log)

    verify = _execute(
        [tc.apksigner, "verify", "--print-certs", signed], log, capture=True
    )
    if verify.returncode != 0:
        raise BuildError(
            f"Falha ao verificar assinatura ({verify.returncode}):"
            f"\n{_tail(verify.stdout or '')}"
        )
    log.detail(verify.stdout or "")
    return signed


def _acquire_build_lock(work: Path) -> Path:
    """Evita que dois builds limpem o mesmo work dir ao mesmo tempo."""
    try:
        work.parent.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise BuildError(f"Falha ao criar {work.parent}: {exc}") from exc
    lock = work.parent / f".{work.name}.apkfast.lock"
    for _ in range(2):
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        except FileExistsError:
            try:
                pid = int(lock.read_text().strip())
                os.kill(pid, 0)
            except (OSError, ValueError):
                # Lock órfão (processo morto ou arquivo corrompido).
                try:
                    lock.unlink()
                except OSError:
                    pass
                continue
            raise BuildError(
                f"Já existe um build em andamento em {work} (PID {pid}). "
                f"Se o processo terminou, remova {lock}."
            )
        except OSError as exc:
            raise BuildError(f"Falha ao criar lock de build {lock}: {exc}") from exc
        with os.fdopen(fd, "w") as stream:
            stream.write(str(os.getpid()))
        return lock
    raise BuildError(f"Não foi possível obter o lock de build {lock}.")


def _release_build_lock(lock: Path) -> None:
    # Melhor esforço: nunca remove o lock de outro processo.
    try:
        if lock.read_text().strip() == str(os.getpid()):
            lock.unlink()
    except OSError:
        pass


def _build(options: BuildOptions) -> Path:
    log = options.log
    assert log is not None
    started_total = time.perf_counter()
    options.built_aab = None  # não herda resultado de um build anterior
    project = options.project.expanduser().resolve()
    if not project.is_dir():
        raise BuildError(f"Diretório de projeto não existe: {project}")

    manifest_path = project / "AndroidManifest.xml"
    if not manifest_path.is_file():
        raise BuildError(f"AndroidManifest.xml não encontrado em {project}")
    manifest = parse_manifest(manifest_path)

    # Keystore/version code: CLI > variáveis de ambiente > padrões seguros.
    try:
        resolved_signing = signing_mod.resolve_keystore_options(
            keystore=options.keystore,
            store_pass=options.keystore_password,
            key_pass=options.key_password,
            key_alias=options.key_alias,
            release=options.release,
        )
    except signing_mod.SigningError as exc:
        raise BuildError(str(exc)) from exc
    options.keystore = resolved_signing.keystore
    options.keystore_password = resolved_signing.store_pass
    options.key_password = resolved_signing.key_pass
    options.key_alias = resolved_signing.key_alias

    try:
        version_code = signing_mod.next_version_code(
            manifest_path,
            strategy=options.version_code_strategy,
            explicit=options.version_code,
            timestamp=options.timestamp_version_code,
        )
    except signing_mod.SigningError as exc:
        # Manifesto sem versionCode é comum em exemplos/testes: usa o mesmo
        # padrão do aapt2 (1). Outros erros continuam bloqueando o build.
        if (
            options.version_code_strategy == "manifest+1"
            and "sem android:versionCode" in str(exc)
            and options.version_code is None
            and not options.timestamp_version_code
        ):
            version_code = 1
        else:
            raise BuildError(str(exc)) from exc
    manifest.version_code = str(version_code)

    # Config do projeto (apkfast.json): bibliotecas próprias podem ser
    # declaradas aqui; flags de CLI têm precedência.
    config = _read_project_config(project)
    if not options.libraries:
        raw_libs = config.get("libraries", [])
        if raw_libs is not None:
            if not isinstance(raw_libs, list) or not all(isinstance(x, str) for x in raw_libs):
                raise BuildError("'libraries' em apkfast.json deve ser uma lista de textos.")
            options.libraries = list(raw_libs)
    if options.libs_dir is None and config.get("libsDir"):
        raw_dir = config["libsDir"]
        if not isinstance(raw_dir, str):
            raise BuildError("'libsDir' em apkfast.json deve ser um texto.")
        options.libs_dir = Path(raw_dir)
    if options.incremental is None:
        raw_incremental = config.get("incremental", True)
        if not isinstance(raw_incremental, bool):
            raise BuildError("'incremental' em apkfast.json deve ser true ou false.")
        options.incremental = raw_incremental
    if "cleanCache" in config:
        if not isinstance(config["cleanCache"], bool):
            raise BuildError("'cleanCache' em apkfast.json deve ser true ou false.")
        options.clean_cache = options.clean_cache or config["cleanCache"]
    if options.cache_dir is None and config.get("cacheDir"):
        raw = config["cacheDir"]
        if not isinstance(raw, str) or not raw.strip():
            raise BuildError("'cacheDir' em apkfast.json deve ser um texto não vazio.")
        options.cache_dir = Path(raw)
    if options.cache_mode is None and config.get("cacheMode"):
        raw = config["cacheMode"]
        if not isinstance(raw, str) or raw.lower() not in ("trusted", "verify"):
            raise BuildError("'cacheMode' deve ser 'trusted' ou 'verify'.")
        options.cache_mode = raw.lower()
    if not options.cache_namespace and config.get("cacheNamespace"):
        raw = config["cacheNamespace"]
        if not isinstance(raw, str):
            raise BuildError("'cacheNamespace' deve ser um texto.")
        options.cache_namespace = raw
    if options.require_cache_signature is None and "requireCacheSignature" in config:
        raw = config["requireCacheSignature"]
        if not isinstance(raw, bool):
            raise BuildError("'requireCacheSignature' deve ser true ou false.")
        options.require_cache_signature = raw

    abis = options.selected_abis()
    if not abis:
        raise BuildError("Nenhuma ABI selecionada.")
    invalid = [abi for abi in abis if abi not in ALL_ABIS]
    if invalid:
        raise BuildError(
            f"ABI(s) inválida(s): {', '.join(invalid)}. "
            f"Use: {', '.join(ALL_ABIS)}"
        )
    duplicates = sorted({abi for abi in abis if abis.count(abi) > 1})
    if duplicates:
        raise BuildError(f"ABI(s) repetida(s): {', '.join(duplicates)}.")

    want_aab = options.aab or options.aab_only
    if want_aab:
        try:
            options.bundletool_version = bundle_mod.validate_version(
                options.bundletool_version
            )
        except bundle_mod.BundleError as exc:
            raise BuildError(str(exc)) from exc

    tc = detect(
        abi=abis[0],
        api=options.api,
        sdk_dir=options.sdk_dir,
        ndk_dir=options.ndk_dir,
        build_tools=options.build_tools,
    )

    # Lockfile de toolchain: uma vez presente, a divergência falha o build
    # (ou apenas avisa com --ignore-lock). Nada aqui altera cache/link/imagem.
    try:
        lock_doc = lock_mod.load(options.lock_path or project)
        if lock_doc is not None:
            mismatches = lock_mod.verify(
                lock_doc,
                lock_mod.facts(tc, api=options.api),
                ignore=options.ignore_lock,
            )
            if mismatches:
                log.warn(
                    "[!] Lockfile divergente (--ignore-lock): "
                    + "; ".join(mismatches)
                )
            else:
                log.detail("[*] Lockfile de toolchain conferido.")
    except lock_mod.LockfileError as exc:
        raise BuildError(str(exc)) from exc

    work = (options.build_dir or (project / "build" / "apkfast")).expanduser().resolve()
    # Nunca permite que a limpeza apague o próprio projeto ou um ancestral.
    if work == project or work in project.parents:
        raise BuildError(
            f"--build-dir não pode ser o projeto nem um ancestral dele: {work}"
        )
    # Cache compartilhado precisa de locks próprios, adquiridos antes do lock
    # de work (que protege apenas _clean()).
    try:
        ctx = cache_mod.resolve_context(
            project,
            package=manifest.package,
            lib_name=_native_lib_name(manifest, project),
            abis=abis,
            cache_dir=options.cache_dir,
            cache_mode=options.cache_mode,
            namespace=options.cache_namespace,
            require_signature=bool(options.require_cache_signature),
        )
    except cache_mod.CacheError as exc:
        raise BuildError(str(exc)) from exc
    if ctx.root == work or work in ctx.root.parents:
        raise BuildError(
            f"--cache-dir não pode ficar dentro de --build-dir: {ctx.root} dentro de {work}"
        )
    options.cache_ctx = ctx
    options.timings_ms["toolchain_ms"] = int(
        round((time.perf_counter() - started_total) * 1000)
    )
    if not ctx.writable:
        log.warn(f"[!] Cache somente leitura ({ctx.root}); usando modo verify sem gravação.")

    cache_locks = cache_mod.acquire_all(ctx, abis)
    try:
        if options.clean_cache:
            # Limpa primeiro as entradas novas (para também remover blobs
            # órfãos) e depois o cache C/C++ por ABI, como antes.
            res_cache.clear_all_resource_caches(ctx)
            for abi in abis:
                cache_mod.clear_abi(ctx, abi)
        # Validações e detecção da toolchain terminaram antes de qualquer limpeza.
        lock = _acquire_build_lock(work)
        try:
            _clean(work)
            (work / "apk").mkdir(parents=True, exist_ok=True)

            log.info("[*] Toolchain:")
            for line in tc.summary().splitlines():
                log.detail(f"    {line}")

            with _timed(options, "resources_ms"):
                flats = _compile_resources(
                    tc, project, work / "res", work, log, options, manifest
                )

            with _timed(options, "package_ms"):
                dex = _compile_java(tc, options, work, log, manifest=manifest)

            with _timed(options, "native_ms"):
                for abi in abis:
                    abi_tc = tc.for_abi(abi)
                    lib, extra_libs = _native_build(
                        abi_tc, options, project, work, manifest
                    )
                    _stage_native_lib(work, abi, lib, extra_libs)

            suffix = "all" if len(abis) > 1 else abis[0]
            if options.release:
                suffix += "-release"
            apk_result: Path | None = None

            if not options.aab_only:
                log.info("[*] Empacotando APK...")
                apk_root = work / "apk"
                unsigned = apk_root / "unsigned_unaligned.apk"
                signed = apk_root / "signed.apk"
                incremental = options.incremental is not False and ctx is not None
                lib_name = _native_lib_name(manifest, project)
                # A keystore precisa existir antes do cálculo da chave: o
                # fingerprint entra no apk_key (e nunca volta ao report).
                keystore = _resolve_keystore(tc, options, log)
                options.cache_keystore_fingerprint = res_cache.keystore_fingerprint_info(
                    keystore
                )
                signed_key = ""
                link_key = ""
                signed_ready = False
                if incremental:
                    assert ctx is not None
                    signed_key = res_cache.apk_key(
                        project, manifest, options, tc, ctx,
                        apk_root=apk_root, lib_name=lib_name,
                        keystore=keystore, abis=abis, signed=True,
                    )
                    link_key = res_cache.apk_key(
                        project, manifest, options, tc, ctx,
                        apk_root=apk_root, lib_name=lib_name,
                        abis=abis, signed=False,
                    )
                    try:
                        with _timed(options, "sign_ms"):
                            res_cache.load_apk(ctx, signed_key, signed)
                        signed_ready = True
                        _cache_event(
                            options, "apk", "hit",
                            "APK assinado reaproveitado (keystore igual)", signed_key,
                        )
                        log.info("[*] APK assinado reaproveitado do cache.")
                    except (res_cache.CacheMiss, OSError) as exc:
                        reason = str(exc)
                        new_fp = str(
                            options.cache_keystore_fingerprint.get("fingerprint", "")
                        )
                        for record in res_cache.apk_records(ctx):
                            meta = record.get("metadata", {})
                            old_fp = str(meta.get("fingerprint", ""))
                            if (
                                meta.get("baseKey") == link_key
                                and old_fp
                                and new_fp
                                and old_fp != new_fp
                            ):
                                reason = (
                                    f"keystore diferente (salvo={old_fp[:8]}, "
                                    f"atual={new_fp[:8]})"
                                )
                                break
                        _cache_event(options, "apk", "miss", reason, signed_key)
                else:
                    _cache_event(
                        options, "apk", "miss", "incremental desligado (--no-incremental)"
                    )

                if signed_ready:
                    # aapt2 link, zipalign e apksigner não rodam: a chave do APK
                    # assinado já cobre resource/native/dex e a keystore.
                    _cache_event(
                        options, "link", "hit",
                        "pulado: APK assinado reaproveitado", link_key,
                    )
                else:
                    unsigned_ready = False
                    with _timed(options, "package_ms"):
                        if incremental:
                            assert ctx is not None
                            try:
                                res_cache.load_apk(
                                    ctx, link_key, unsigned, unsigned=True
                                )
                                unsigned_ready = True
                                _cache_event(
                                    options, "link", "hit",
                                    "APK unsigned reaproveitado do cache", link_key,
                                )
                            except (res_cache.CacheMiss, OSError) as exc:
                                _cache_event(
                                    options, "link", "miss", str(exc), link_key
                                )
                        else:
                            _cache_event(
                                options, "link", "miss",
                                "incremental desligado (--no-incremental)",
                            )
                        if not unsigned_ready:
                            unsigned = _link_apk(
                                tc, options, work, manifest_path, manifest, flats, dex
                            )
                            _add_extra_files_with_zip(unsigned, apk_root)
                            if incremental:
                                assert ctx is not None
                                res_cache.store_apk(
                                    ctx, link_key, unsigned, unsigned=True
                                )

                    log.info("[*] Alinhando e assinando...")
                    with _timed(options, "sign_ms"):
                        signed = _align_and_sign(tc, options, work, unsigned)
                    if incremental:
                        assert ctx is not None
                        res_cache.store_apk(
                            ctx,
                            signed_key,
                            signed,
                            metadata={
                                **options.cache_keystore_fingerprint,
                                "baseKey": link_key,
                            },
                        )

                output = options.output or (
                    project / "dist" / f"{manifest.package.split('.')[-1]}-{suffix}.apk"
                )
                output = output.expanduser().resolve()
                if output == project or output in project.parents:
                    raise BuildError(
                        f"--output não pode ser o projeto nem um ancestral dele: {output}"
                    )
                try:
                    output.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(signed, output)
                    idsig = signed.with_name(signed.name + ".idsig")
                    if idsig.is_file():
                        shutil.copy2(idsig, Path(str(output) + ".idsig"))
                except OSError as exc:
                    raise BuildError(f"Falha ao gravar APK final {output}: {exc}") from exc
                size = output.stat().st_size / 1024
                log.info(f"[✓] APK final: {output} ({size:.1f} KiB)")
                apk_result = output

            if want_aab:
                keystore = _resolve_keystore(tc, options, log)
                options.cache_keystore_fingerprint = res_cache.keystore_fingerprint_info(
                    keystore
                )
                aab_output = bundle_mod.default_aab_path(
                    manifest.package,
                    suffix,
                    project / "dist",
                    apk_output=options.output,
                    explicit=options.aab_output,
                ).expanduser().resolve()
                if aab_output == project or aab_output in project.parents:
                    raise BuildError(
                        f"--aab-output não pode ser o projeto nem um ancestral dele: {aab_output}"
                    )

                incremental_aab = options.incremental is not False and ctx is not None
                aab_key = ""
                aab_ready = False
                if incremental_aab:
                    assert ctx is not None
                    aab_key = res_cache.aab_key(
                        project, manifest, options, tc, ctx,
                        apk_root=work / "apk",
                        lib_name=_native_lib_name(manifest, project),
                        keystore=keystore,
                        abis=abis,
                        bundletool_version=options.bundletool_version,
                        precomputed_dex_key=options.cache_dex_key or None,
                    )
                    options.cache_aab_key = aab_key
                    try:
                        with _timed(options, "package_ms"):
                            res_cache.load_aab(ctx, aab_key, aab_output)
                        aab_ready = True
                        _cache_event(
                            options, "aab", "hit",
                            "AAB assinado reaproveitado (keystore/bundletool iguais)",
                            aab_key,
                        )
                        log.info("[*] AAB assinado reaproveitado do cache.")
                    except (res_cache.CacheMiss, OSError) as exc:
                        reason = str(exc)
                        new_fp = str(
                            options.cache_keystore_fingerprint.get("fingerprint", "")
                        )
                        for record in res_cache.aab_records(ctx):
                            meta = record.get("metadata", {}) or {}
                            old_fp = str(meta.get("fingerprint", ""))
                            if old_fp and new_fp and old_fp != new_fp:
                                reason = (
                                    f"keystore diferente (salvo={old_fp[:8]}, "
                                    f"atual={new_fp[:8]})"
                                )
                                break
                        _cache_event(options, "aab", "miss", reason, aab_key)
                else:
                    _cache_event(
                        options, "aab", "miss",
                        "incremental desligado (--no-incremental)",
                    )

                if not aab_ready:
                    log.info("[*] Gerando AAB...")
                    log.warn(
                        "[!] AAB será assinado com a MESMA keystore do APK "
                        f"({keystore}, alias {options.key_alias}). "
                        "Para a Play Store use uma upload key própria; se ela estiver "
                        "inscrita no Play App Signing, NÃO use a chave de app signing."
                    )
                    if options.keystore is None:
                        log.warn(
                            "[!] Keystore de debug: o AAB resultante NÃO pode ser publicado "
                            "na Play Store (use --release --keystore ...)."
                        )
                    try:
                        bundletool = bundle_mod.ensure_bundletool(
                            options.bundletool_version, quiet=log.quiet
                        )
                        proto = _link_proto(
                            tc, options, work, manifest_path, manifest, flats
                        )
                        assets_dir = options.assets_dir or (project / "assets")
                        unsigned_aab = bundle_mod.build_aab(
                            work=work,
                            proto_apk=proto,
                            bundletool=bundletool,
                            dex_dir=work / "apk",
                            lib_dir=work / "apk" / "lib",
                            assets_dir=assets_dir if assets_dir.is_dir() else None,
                            quiet=log.quiet,
                            java=tc.java,
                        )
                        signed_aab = bundle_mod.sign_aab(
                            unsigned_aab,
                            keystore=keystore,
                            store_password=options.keystore_password,
                            key_alias=options.key_alias,
                            key_password=options.key_password,
                            output=work / "aab" / "signed.aab",
                            quiet=log.quiet,
                        )
                        bundle_mod.verify_aab(
                            signed_aab,
                            bundletool=bundletool,
                            work=work / "aab",
                            aapt2=tc.aapt2,
                            keystore=keystore,
                            store_password=options.keystore_password,
                            key_alias=options.key_alias,
                            key_password=options.key_password,
                            quiet=log.quiet,
                            java=tc.java,
                        )
                        if aab_key and ctx is not None:
                            res_cache.store_aab(
                                ctx,
                                aab_key,
                                signed_aab,
                                metadata=options.cache_keystore_fingerprint,
                            )
                        aab_output.parent.mkdir(parents=True, exist_ok=True)
                        shutil.copy2(signed_aab, aab_output)
                    except bundle_mod.BundleError as exc:
                        raise BuildError(str(exc)) from exc
                    except OSError as exc:
                        raise BuildError(f"Falha ao gravar AAB final: {exc}") from exc
                options.built_aab = aab_output
                size = aab_output.stat().st_size / 1024
                log.info(f"[✓] AAB final: {aab_output} ({size:.1f} KiB)")

            result = apk_result if apk_result is not None else options.built_aab
            if result is None:
                raise BuildError("Nenhum artefato foi gerado (verifique --aab/--aab-only).")
            return result
        finally:
            _release_build_lock(lock)
    finally:
        cache_mod.release_all(cache_locks)


def build(options: BuildOptions) -> Path:
    """Entrada pública: mede o tempo total mesmo em caso de erro."""
    started = time.perf_counter()
    try:
        return _build(options)
    finally:
        options.timings_ms["total_ms"] = int(
            round((time.perf_counter() - started) * 1000)
        )
