"""The no-Gradle APK build pipeline."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from .manifest import ManifestInfo, is_native_activity, parse_manifest
from .toolchain import NDK_ARCH_DIR, Toolchain, detect

DEFAULT_IGNORED_DIRS = {".git", "build", "obj", ".apkfast", "gradle", ".gradle"}
ALL_ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")


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
    keystore_password: str = "android"
    key_alias: str = "androiddebugkey"
    key_password: str = "android"
    sdk_dir: str | None = None
    ndk_dir: str | None = None
    build_tools: str | None = None
    extra_cflags: list[str] = field(default_factory=list)
    extra_ldflags: list[str] = field(default_factory=list)
    sources: list[str] = field(default_factory=list)
    assets_dir: Path | None = None
    verbose: bool = False
    quiet: bool = False
    log: Logger | None = None

    def __post_init__(self) -> None:
        if self.log is None:
            self.log = Logger(quiet=self.quiet, verbose=self.verbose)

    def selected_abis(self) -> list[str]:
        if self.abis:
            return list(self.abis)
        if self.abi == "all":
            return list(ALL_ABIS)
        return [self.abi]


def _run(cmd: list[str], log: Logger, *, cwd: Path | None = None) -> subprocess.CompletedProcess:
    log.command(cmd)
    result = subprocess.run([str(c) for c in cmd], cwd=cwd)
    if result.returncode != 0:
        raise BuildError(
            f"Comando falhou ({result.returncode}): {' '.join(str(c) for c in cmd)}"
        )
    return result


def _clean(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


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
    tc: Toolchain, project: Path, staged_res: Path, work: Path, log: Logger
) -> list[Path]:
    res_dir = project / "res"
    if not res_dir.is_dir() or not any(res_dir.iterdir()):
        return []
    compiled_dir = work / "compiled_res"
    compiled_dir.mkdir(parents=True, exist_ok=True)
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
        flat_files.append(output)
    return flat_files


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

    # Injeta o TU com as implementações third-party do nativeapp (stb_image,
    # stb_truetype, stb_vorbis, dr_mp3) exatamente uma vez por build.
    if options.use_nativeapp:
        # Detecta raylib antes da injeção (ela já define os símbolos stbi_*).
        uses_raylib_here = any(
            "raylib.h" in s.read_text(errors="ignore") for s in sources if s.is_file()
        )
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

    native_dir = work / "native" / NDK_ARCH_DIR[tc.abi]
    native_dir.mkdir(parents=True, exist_ok=True)
    objects: list[Path] = []

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
    source_text = "\n".join(
        s.read_text(errors="ignore") for s in sources if s.is_file()
    )
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
        return tc.clang_target_flags(explicit_target=compiler.name in ("clang", "clang++"))

    # Diretório da API nativeapp (headers ficam em <raiz do apkfast>/include).
    root = Path(__file__).resolve().parent.parent
    api_include = root / "include"
    raylib_dir = root / "vendor" / "raylib"
    raylib_static = raylib_dir / "lib" / f"libraylib-{tc.abi}.a"

    common_flags = [
        *optimization.split(),
        "-Wall",
        f"-I{project}",
        f"-I{project / 'jni'}",
        f"-I{project / 'src' / 'main' / 'cpp'}",
        f"-I{tc.native_app_glue_dir()}",
        *( [f"-I{api_include}"] if options.use_nativeapp and api_include.is_dir() else [] ),
        *( [f"-I{raylib_dir / 'include'}"] if uses_raylib and raylib_dir.is_dir() else [] ),
        *( [f"-I{root / 'vendor' / 'sdl2' / 'include'}"]
           if uses_sdl2 and (root / 'vendor' / 'sdl2' / 'include').is_dir() else [] ),
        *( [f"-I{root / 'vendor' / 'sdl3' / 'include'}"]
           if uses_sdl3 and (root / 'vendor' / 'sdl3' / 'include').is_dir() else [] ),
        *options.extra_cflags,
    ]

    log.info(f"[*] Compilando {len(sources)} arquivo(s) nativo(s) para {tc.abi}")
    for index, source in enumerate(sources):
        is_cpp = source.suffix in (".cpp", ".cc", ".cxx", ".cxx")
        compiler = tc.clang(cpp=is_cpp)
        obj = native_dir / f"obj_{index}_{source.stem}.o"
        cmd = [compiler, "-c", str(source), "-o", str(obj)]
        cmd += compiler_flags(compiler)
        cmd += base_flags
        cmd += ["-std=c++17"] if is_cpp else ["-std=c11"]
        cmd += common_flags
        _run(cmd, log)
        objects.append(obj)

    glue_source = tc.native_app_glue_dir() / "android_native_app_glue.c"
    glue_obj = native_dir / "android_native_app_glue.o"
    # O raylib fornece android_main() (que chama main()), mas não fornece
    # ANativeActivity_onCreate — esse símbolo vem do glue do NDK. Portanto
    # compilamos o glue normalmente; o usuário deve definir main(), não
    # android_main(), em projetos raylib.
    # SDL3 usa SDLActivity (Java), que já fornece ANativeActivity_onCreate e
    # chama android_main() diretamente. Compilar o glue do NDK aqui causaria
    # referência dupla a android_main.
    if glue_source.is_file() and not uses_sdl3 and not uses_sdl2:
        glue_compiler = tc.clang()
        cmd = [glue_compiler, "-c", str(glue_source), "-o", str(glue_obj)]
        cmd += compiler_flags(glue_compiler)
        cmd += base_flags + ["-std=c11"] + common_flags
        _run(cmd, log)
        objects.append(glue_obj)

    if is_native_activity(manifest) and not uses_raylib and not uses_sdl3 and not uses_sdl2 and not any(
        "android_native_app_glue" in o.name for o in objects
    ):
        log.warn("[!] Aviso: NativeActivity declarada mas native_app_glue não foi compilado.")

    output = native_dir / f"lib{_native_lib_name(manifest, project)}.so"
    has_cpp = any(s.suffix in (".cpp", ".cc", ".cxx") for s in sources)
    linker = tc.clang(cpp=has_cpp)
    cmd = [linker, "-shared", "-o", str(output)]
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
        "-lm",
        "-ldl",
        "-landroid",
        "-llog",
        "-Wl,--no-as-needed", "-lOpenSLES",
        *( ["-laaudio"] if tc.api >= 26 else [] ),
        "-Wl,--as-needed",
        *( ["-lEGL", "-lGLESv2"] if uses_gles else [] ),
        *options.extra_ldflags,
    ]
    extra_libs: list[Path] = []
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
    _run(cmd, log)

    if options.optimize != "none" or options.obfuscate:
        strip = tc.ndk_toolchain_bin / "llvm-strip"
        if strip.is_file():
            strip_args = [strip, "--strip-all" if options.obfuscate else "--strip-unneeded", str(output)]
            _run(strip_args, log)

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
    config = project / "apkfast.json"
    if config.is_file():
        import json

        try:
            data = json.loads(config.read_text())
            name = data.get("nativeLibraryName")
            if name:
                return str(name)
        except Exception:
            pass
    return "main"


def _stage_native_lib(
    work: Path, abi: str, lib: Path | None, extra_libs: list[Path]
) -> Path | None:
    if lib is None:
        return None
    target = work / "apk" / "lib" / abi
    target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(lib, target / lib.name)
    for extra in extra_libs:
        shutil.copy2(extra, target / extra.name)
    return target


def _find_tool(tc: Toolchain, name: str) -> Path | None:
    """Procura uma ferramenta no SDK (cmdline-tools/bin, build-tools) ou PATH."""
    import shutil

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
    tc: Toolchain, options: BuildOptions, work: Path, log: Logger
) -> Path | None:
    """Compila .java para classes e depois usa R8/D8 quando presente.

    Retorna o caminho do dex gerado, ou None se não houver código Java.
    """
    java_sources = _iter_files(options.project, ".java", DEFAULT_IGNORED_DIRS)
    jars = _iter_files(options.project, ".jar", DEFAULT_IGNORED_DIRS)

    if not java_sources and not jars:
        return None

    classes = work / "java_classes"
    classes.mkdir(parents=True, exist_ok=True)
    javac = _find_tool(tc, "javac") or Path("javac")
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

    dex = work / "apk" / "classes.dex"
    r8 = _find_tool(tc, "r8")
    d8 = _find_tool(tc, "d8") or tc.d8
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
    import zipfile

    extra_files = sorted(extra_root.rglob("*.so")) + sorted(extra_root.rglob("*.dex"))
    if not extra_files:
        return
    tmp = apk.with_suffix(".withextra.apk")
    with zipfile.ZipFile(apk, "r") as src, zipfile.ZipFile(
        tmp, "w", compression=zipfile.ZIP_DEFLATED
    ) as dst:
        for info in src.infolist():
            dst.writestr(info, src.read(info.filename))
        for extra in extra_files:
            arcname = str(extra.relative_to(extra_root))
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
    try:
        _run(align_cmd, log)
    except BuildError:
        log.warn("[!] zipalign sem suporte a -P 16; usando alinhamento 4.")
        _run([tc.zipalign, "-f", "4", str(unsigned), str(aligned)], log)

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

    signed = work / "apk" / "signed.apk"
    cmd = [
        tc.apksigner,
        "sign",
        "--ks",
        str(keystore),
        "--ks-pass",
        f"pass:{options.keystore_password}",
        "--ks-key-alias",
        options.key_alias,
        "--key-pass",
        f"pass:{options.key_password}",
        "--v1-signing-enabled",
        "true" if options.min_sdk < 24 else "false",
        "--v2-signing-enabled",
        "true",
        "--out",
        str(signed),
        str(aligned),
    ]
    _run(cmd, log)

    verify = subprocess.run(
        [str(tc.apksigner), "verify", "--print-certs", str(signed)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if verify.returncode != 0:
        raise BuildError(f"Falha ao verificar assinatura:\n{verify.stdout}")
    log.detail(verify.stdout)
    return signed


def build(options: BuildOptions) -> Path:
    log = options.log
    assert log is not None
    project = options.project.expanduser().resolve()
    if not project.is_dir():
        raise BuildError(f"Diretório de projeto não existe: {project}")

    manifest_path = project / "AndroidManifest.xml"
    if not manifest_path.is_file():
        raise BuildError(f"AndroidManifest.xml não encontrado em {project}")
    manifest = parse_manifest(manifest_path)

    abis = options.selected_abis()
    if not abis:
        raise BuildError("Nenhuma ABI selecionada.")
    invalid = [abi for abi in abis if abi not in ALL_ABIS]
    if invalid:
        raise BuildError(
            f"ABI(s) inválida(s): {', '.join(invalid)}. "
            f"Use: {', '.join(ALL_ABIS)}"
        )

    tc = detect(
        abi=abis[0],
        api=options.api,
        sdk_dir=options.sdk_dir,
        ndk_dir=options.ndk_dir,
        build_tools=options.build_tools,
    )

    work = (options.build_dir or (project / "build" / "apkfast")).expanduser().resolve()
    # Nunca permite que a limpeza apague o próprio projeto ou um ancestral.
    if work == project or work in project.parents:
        raise BuildError(
            f"--build-dir não pode ser o projeto nem um ancestral dele: {work}"
        )
    _clean(work)
    (work / "apk").mkdir(parents=True, exist_ok=True)

    log.info("[*] Toolchain:")
    for line in tc.summary().splitlines():
        log.detail(f"    {line}")

    flats = _compile_resources(tc, project, work / "res", work, log)
    dex = _compile_java(tc, options, work, log)

    for abi in abis:
        abi_tc = tc.for_abi(abi)
        lib, extra_libs = _native_build(abi_tc, options, project, work, manifest)
        _stage_native_lib(work, abi, lib, extra_libs)

    log.info("[*] Empacotando APK...")
    unsigned = _link_apk(tc, options, work, manifest_path, manifest, flats, dex)
    _add_extra_files_with_zip(unsigned, work / "apk")

    log.info("[*] Alinhando e assinando...")
    signed = _align_and_sign(tc, options, work, unsigned)

    suffix = "all" if len(abis) > 1 else abis[0]
    if options.release:
        suffix += "-release"
    output = options.output or (project / "dist" / f"{manifest.package.split('.')[-1]}-{suffix}.apk")
    output = output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(signed, output)
    size = output.stat().st_size / 1024
    log.info(f"[✓] APK final: {output} ({size:.1f} KiB)")
    return output
