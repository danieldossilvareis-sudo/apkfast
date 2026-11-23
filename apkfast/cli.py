"""Command-line interface for apkfast."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

from . import __version__
from .builder import BuildOptions, BuildError, build
from .toolchain import detect

BANNER = r"""
  _    ____  __  __  ___  _   _   _
 / \  |  _ \|  \/  |/ _ \| | | | / \
/ _ \ | |_) | |\/| | | | | | | |/ _ \
/ ___ \|  __/| |  | | |_| | |_| / ___ \
/_/   \_\_|   |_|  |_|\___/ \___/_/   \_
"""


def _add_quiet(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "-q", "--quiet", dest="cmd_quiet", action="store_true",
        help="silencioso: mostra só erros",
    )


def _quiet(args: argparse.Namespace) -> bool:
    """Única fonte de verdade para o modo silencioso.

    Os subcomandos usam ``cmd_quiet`` para não sobrescrever a flag global
    durante o parse; ``build_quiet`` é aceito apenas por compatibilidade com
    Namespace construído por código externo.
    """
    return bool(
        getattr(args, "quiet", False)
        or getattr(args, "cmd_quiet", False)
        or getattr(args, "build_quiet", False)
    )


def _say(args: argparse.Namespace, message: str) -> None:
    if not _quiet(args):
        print(message)


def _fail(message: str) -> int:
    print(f"[✗] {message}", file=sys.stderr)
    return 1


def _add_build_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("project", nargs="?", default=".", help="diretório do projeto")
    parser.add_argument("-o", "--output", help="caminho do APK final")
    parser.add_argument("--build-dir", help="diretório de arquivos temporários")
    parser.add_argument(
        "--no-incremental", dest="incremental", action="store_false", default=None,
        help="desliga o cache incremental (recompila tudo)",
    )
    parser.add_argument(
        "--clean-cache", action="store_true",
        help="apaga o cache incremental antes do build",
    )
    parser.add_argument(
        "--cache-dir",
        help="raiz do cache (padrão: ~/.cache/apkfast; pode ser NFS/sshfs)",
    )
    parser.add_argument(
        "--cache-mode", choices=("trusted", "verify"), default=None,
        help="modo de verificação do cache (padrão: trusted local, verify se --cache-dir)",
    )
    parser.add_argument(
        "--cache-namespace", default="",
        help="isola projetos com o mesmo package (ex.: monorepo)",
    )
    parser.add_argument(
        "--require-cache-signature", action="store_true", default=None,
        help="exige assinatura HMAC em bundles importados",
    )
    parser.add_argument(
        "--abi",
        default="arm64-v8a",
        help="ABI alvo: arm64-v8a, armeabi-v7a, x86, x86_64 ou all (default: arm64-v8a)",
    )
    parser.add_argument(
        "--abis",
        default="",
        help="lista de ABIs separadas por vírgula (ex: arm64-v8a,armeabi-v7a)",
    )
    parser.add_argument("--api", type=int, default=36, help="API do NDK/clang (default: 36)")
    parser.add_argument("--min-sdk", type=int, default=21, help="minSdkVersion (default: 21)")
    parser.add_argument("--target-sdk", type=int, default=36, help="targetSdkVersion (default: 36)")
    parser.add_argument("--optimize", choices=("none", "release", "size"), default="release")
    parser.add_argument(
        "--release",
        action="store_true",
        help="modo release: exige --keystore (não usa keystore debug)",
    )
    parser.add_argument(
        "--obfuscate",
        action="store_true",
        help="ofuscação: esconde símbolos nativos, strip total e R8 para Java",
    )
    parser.add_argument(
        "--no-nativeapp",
        action="store_true",
        help="não adiciona automaticamente os headers da API nativeapp",
    )
    parser.add_argument(
        "--libcxx-shared",
        action="store_true",
        help="usa libc++_shared.so (embutido no APK) em vez do estático",
    )
    parser.add_argument("--sdk-dir", help="caminho explícito do Android SDK")
    parser.add_argument("--ndk-dir", help="caminho explícito do Android NDK")
    parser.add_argument("--build-tools", help="versão dos build-tools (ex: 37.0.0)")
    parser.add_argument("--keystore", help="keystore para assinar (default: ~/.apkfast/debug.jks)")
    parser.add_argument("--store-pass", default="android", help="senha do keystore")
    parser.add_argument("--alias", default="androiddebugkey", help="alias da chave")
    parser.add_argument("--key-pass", default="android", help="senha da chave")
    parser.add_argument("--cflags", default="", help="flags extras para C/C++")
    parser.add_argument("--ldflags", default="", help="flags extras para o linker")
    parser.add_argument("--src", action="append", default=[], help="arquivo fonte (repitível)")
    parser.add_argument(
        "--lib", action="append", default=[], dest="libraries",
        help="biblioteca .a/.so própria (repitível); ex.: libs/arm64-v8a/libfoo.a",
    )
    parser.add_argument(
        "--libs-dir", help="raiz de bibliotecas (default: <projeto>/libs, com <abi>/)"
    )
    parser.add_argument("--assets", help="diretório de assets (default: <projeto>/assets)")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "-q", "--quiet", dest="cmd_quiet", action="store_true", help="silencioso: mostra só erros"
    )


def _cmd_build(args: argparse.Namespace) -> int:
    from .builder import Logger

    log = Logger(quiet=_quiet(args), verbose=args.verbose)
    options = BuildOptions(
        project=Path(args.project),
        output=Path(args.output) if args.output else None,
        build_dir=Path(args.build_dir) if args.build_dir else None,
        abi=args.abi,
        abis=[a.strip() for a in args.abis.split(",") if a.strip()],
        api=args.api,
        min_sdk=args.min_sdk,
        target_sdk=args.target_sdk,
        optimize=args.optimize,
        release=args.release,
        obfuscate=args.obfuscate,
        libcxx_shared=args.libcxx_shared,
        use_nativeapp=not args.no_nativeapp,
        keystore=Path(args.keystore) if args.keystore else None,
        keystore_password=args.store_pass,
        key_alias=args.alias,
        key_password=args.key_pass,
        extra_cflags=args.cflags.split(),
        extra_ldflags=args.ldflags.split(),
        sources=args.src,
        libraries=args.libraries,
        libs_dir=Path(args.libs_dir) if args.libs_dir else None,
        assets_dir=Path(args.assets) if args.assets else None,
        incremental=args.incremental,
        clean_cache=args.clean_cache,
        cache_dir=Path(args.cache_dir) if args.cache_dir else None,
        cache_mode=args.cache_mode,
        cache_namespace=args.cache_namespace or "",
        require_cache_signature=args.require_cache_signature,
        verbose=args.verbose,
        quiet=_quiet(args),
        log=log,
    )
    try:
        apk = build(options)
    except (BuildError, RuntimeError, OSError) as exc:
        log.error(f"[✗] {exc}")
        return 1
    log.info(f"[✓] Build concluído: {apk}")
    return 0


def _cmd_init(args: argparse.Namespace) -> int:
    from .scaffold import create_project, interactive_config

    target = Path(args.name).expanduser().resolve()
    if target.exists() and not target.is_dir():
        return _fail(f"Caminho não é um diretório: {target}")
    if target.exists() and any(target.iterdir()) and not args.force:
        return _fail(f"Diretório não vazio: {target} (use --force)")

    try:
        config = interactive_config(args, target)
        if config is None:
            return 1
        create_project(target, config)
    except (OSError, ValueError) as exc:
        return _fail(f"Falha ao criar projeto: {exc}")

    _say(args, f"[✓] Projeto criado em {target}")
    _say(args, f"    template: {config.template} | package: {config.package}")
    cli_relative = os.path.relpath(Path(__file__).resolve().parent.parent, target)
    _say(args, f"    cd {target} && python3 {cli_relative}/apkfast.py build")
    if config.template in ("raylib", "sdl2", "sdl3"):
        _say(args, f"    (instale as libs: python3 {cli_relative}/apkfast.py setup-libs)")
    return 0


def _cmd_doctor(args: argparse.Namespace) -> int:
    _say(args, BANNER)
    try:
        tc = detect(abi=args.abi, api=args.api)
    except (RuntimeError, OSError) as exc:
        return _fail(str(exc))
    _say(args, tc.summary())
    _say(args, "\nVerificando ferramentas:")
    for name, path in [
        ("aapt2", tc.aapt2),
        ("zipalign", tc.zipalign),
        ("apksigner", tc.apksigner),
        ("d8", tc.d8),
        ("clang", tc.clang()),
        ("clang++", tc.clang(cpp=True)),
        ("llvm-strip", tc.ndk_toolchain_bin / "llvm-strip"),
        ("java", tc.java),
        ("keytool", tc.keytool),
    ]:
        status = "OK" if path and Path(path).exists() else "FALTANDO"
        _say(args, f"  {status:9} {name:12} {path}")
    return 0


def _cmd_install(args: argparse.Namespace) -> int:
    from .device import DeviceError, install

    apk = Path(args.apk).expanduser().resolve()
    try:
        output = install(
            apk,
            adb_path=args.adb,
            serial=args.device,
            reinstall=not args.no_reinstall,
            allow_downgrade=args.debug,
            grant_permissions=args.grant_permissions,
        )
    except (DeviceError, OSError) as exc:
        return _fail(str(exc))
    _say(args, f"[✓] Instalado em {args.device or 'dispositivo'}: {output}")
    return 0


def _setup_platform_api(args: argparse.Namespace) -> int:
    """API da platform: explícita, ou derivada de --sdk-version (37.0.0 -> 37)."""
    explicit = getattr(args, "platform_api", None)
    if explicit is not None:
        return int(explicit)
    version = str(getattr(args, "sdk_version", "") or "").strip()
    match = re.match(r"(\d+)", version)
    return int(match.group(1)) if match else 36


def _cmd_setup_sdk(args: argparse.Namespace) -> int:
    from .sdk_setup import SDK_REPO, NDK_REPO, SetupError, install_sdk, list_versions

    if args.list_versions:
        try:
            for label, repo in (("SDK", SDK_REPO), ("NDK", NDK_REPO)):
                print(f"{label} ({repo.split('/')[-1]}):")
                for version in list_versions(repo):
                    print(f"  {version}")
        except (SetupError, OSError) as exc:
            return _fail(str(exc))
        return 0

    platform_api = _setup_platform_api(args)
    if platform_api < 1:
        return _fail(f"API da platform inválida: {platform_api}")

    try:
        paths = install_sdk(
            target=Path(args.target) if args.target else None,
            sdk_version=args.sdk_version,
            ndk_version=args.ndk_version,
            platform_api=platform_api,
            force=args.force,
            quiet=_quiet(args),
            assume_yes=args.yes,
        )
    except SetupError as exc:
        return _fail(str(exc))
    except OSError as exc:
        return _fail(f"Falha de filesystem ao instalar SDK/NDK: {exc}")
    except EOFError:
        return _fail("Confirmação não recebida; use -y/--yes para instalar sem perguntar.")
    _say(args, f"[✓] SDK: {paths['sdk']}")
    _say(args, f"[✓] NDK: {paths['ndk']}")
    _say(args, "Rode: python3 apkfast.py doctor")
    return 0


def _cmd_setup_libs(args: argparse.Namespace) -> int:
    """Baixa/compila raylib + SDL2 + SDL3 para as ABIs pedidas."""
    from .raylib import RaylibError, install as raylib_install
    from .sdl import SDLError, install as sdl_install
    from .lunasvg import LunaSVGError, install as lunasvg_install
    from .webp import WebPError, install as webp_install

    if args.abis.strip() == "all":
        from .builder import ALL_ABIS

        abis = list(ALL_ABIS)
    else:
        abis = [a.strip() for a in args.abis.split(",") if a.strip()] or ["arm64-v8a"]
    try:
        lunasvg_install(quiet=_quiet(args))
    except (LunaSVGError, OSError) as exc:
        return _fail(f"LunaSVG: {exc}")
    for abi in abis:
        for name, fn in (
            ("raylib", lambda: raylib_install(abi, api=36, quiet=_quiet(args))),
            ("SDL2", lambda: sdl_install(2, abi, api=21, quiet=_quiet(args))),
            ("SDL3", lambda: sdl_install(3, abi, api=21, quiet=_quiet(args))),
            ("libwebp", lambda: webp_install(abi, api=21, quiet=_quiet(args))),
        ):
            try:
                fn()
            except (RaylibError, SDLError, WebPError, LunaSVGError, RuntimeError, OSError) as exc:
                return _fail(f"{name} ({abi}): {exc}")
    _say(args, "[✓] Bibliotecas prontas.")
    return 0


def _cmd_sdl_install(args: argparse.Namespace) -> int:
    from .sdl import SDLError, install

    try:
        install(args.version, abi=args.abi, api=args.api, quiet=_quiet(args))
    except (SDLError, RuntimeError, OSError) as exc:
        return _fail(str(exc))
    return 0


def _cmd_raylib_install(args: argparse.Namespace) -> int:
    from .raylib import RaylibError, install

    try:
        install(abi=args.abi, api=args.api, quiet=_quiet(args))
    except (RaylibError, RuntimeError, OSError) as exc:
        return _fail(str(exc))
    return 0


def _cmd_lunasvg_install(args: argparse.Namespace) -> int:
    from .lunasvg import LunaSVGError, install

    try:
        install(quiet=_quiet(args))
    except (LunaSVGError, OSError) as exc:
        return _fail(str(exc))
    return 0


def _cmd_webp_install(args: argparse.Namespace) -> int:
    from .webp import WebPError, install

    try:
        install(abi=args.abi, api=args.api, quiet=_quiet(args))
    except (WebPError, RuntimeError, OSError) as exc:
        return _fail(str(exc))
    return 0


def _cmd_logs(args: argparse.Namespace) -> int:
    from .device import DeviceError, find_adb

    try:
        adb = find_adb(args.adb)
        cmd = [str(adb), "logcat"]
        if args.crash:
            cmd += ["-b", "crash"]
        if args.clear:
            subprocess.run([str(adb), "logcat", "-c"], check=False)
        if args.package:
            cmd += ["-s", f"{args.package}:I", "*:E"]
        cmd += ["-d", "-t", str(args.lines)]
        result = subprocess.run(cmd)
        return result.returncode
    except (DeviceError, OSError) as exc:
        return _fail(str(exc))


def _cmd_devices(args: argparse.Namespace) -> int:
    from .device import DeviceError, find_adb, list_devices, restart_server

    try:
        adb = find_adb(args.adb)
        devices = list_devices(adb)
        if not devices and args.restart:
            restart_server(adb)
            devices = list_devices(adb)
    except (DeviceError, OSError) as exc:
        return _fail(str(exc))

    if not devices:
        _say(args, "Nenhum dispositivo conectado.")
        return 0
    for device in devices:
        state = "ONLINE" if device.online else device.state.upper()
        _say(args, f"  {device.serial:20} {state:12} {device.model}")
    return 0


def _cmd_info(args: argparse.Namespace) -> int:
    path = Path(args.apk).expanduser().resolve()
    if not path.is_file():
        return _fail(f"APK não encontrado: {path}")
    try:
        tc = detect()
    except (RuntimeError, OSError) as exc:
        return _fail(str(exc))
    try:
        result = subprocess.run(
            [str(tc.aapt2), "dump", "badging", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    except OSError as exc:
        return _fail(f"Falha ao executar aapt2: {exc}")
    print(result.stdout or "")
    if result.returncode != 0:
        return _fail(f"aapt2 dump badging falhou ({result.returncode})")
    return 0


def _cache_transport_context(args: argparse.Namespace) -> tuple[object, list[str]]:
    """Resolve projeto/manifesto/cache para cache-export/import (sem toolchain)."""
    from . import cache as cache_mod
    from .builder import ALL_ABIS, _native_lib_name, _read_project_config
    from .manifest import parse_manifest

    project = Path(args.project).expanduser().resolve()
    manifest_path = project / "AndroidManifest.xml"
    if not manifest_path.is_file():
        raise ValueError(f"AndroidManifest.xml não encontrado em {project}")
    manifest = parse_manifest(manifest_path)
    config = _read_project_config(project)

    raw_abis = [a.strip() for a in args.abis.split(",") if a.strip()]
    if args.abi == "all" and not raw_abis:
        abis = list(ALL_ABIS)
    elif raw_abis:
        abis = raw_abis
    else:
        abis = [args.abi]
    invalid = [abi for abi in abis if abi not in ALL_ABIS]
    if invalid:
        raise ValueError(f"ABI(s) inválida(s): {', '.join(invalid)}")

    cache_dir = Path(args.cache_dir) if getattr(args, "cache_dir", None) else None
    if cache_dir is None and config.get("cacheDir"):
        cache_dir = Path(config["cacheDir"])
    cache_mode = getattr(args, "cache_mode", None) or config.get("cacheMode")
    namespace = getattr(args, "cache_namespace", "") or config.get("cacheNamespace", "")
    require_sig = getattr(args, "require_cache_signature", None)
    if require_sig is None:
        require_sig = config.get("requireCacheSignature", False)

    ctx = cache_mod.resolve_context(
        project,
        package=manifest.package,
        lib_name=_native_lib_name(manifest, project),
        abis=abis,
        cache_dir=cache_dir,
        cache_mode=cache_mode,
        namespace=namespace or "",
        require_signature=bool(require_sig),
    )
    return ctx, abis


def _cmd_cache_export(args: argparse.Namespace) -> int:
    from . import cache as cache_mod

    try:
        ctx, abis = _cache_transport_context(args)
        locks = cache_mod.acquire_all(ctx, abis)
        try:
            manifest = cache_mod.export_bundle(ctx, Path(args.output), abis)
        finally:
            cache_mod.release_all(locks)
    except (ValueError, cache_mod.CacheError, OSError) as exc:
        return _fail(f"Falha ao exportar cache: {exc}")
    print(
        f"[✓] Bundle: {args.output} "
        f"({len(manifest['blobs'])} blobs, ABIs: {', '.join(sorted(abis))})"
    )
    return 0


def _cmd_cache_import(args: argparse.Namespace) -> int:
    from . import cache as cache_mod

    try:
        ctx, abis = _cache_transport_context(args)
        locks = cache_mod.acquire_all(ctx, abis)
        try:
            stats = cache_mod.import_bundle(
                ctx,
                Path(args.bundle),
                abis=abis,
                force=args.force,
                require_signature=args.require_cache_signature,
            )
        finally:
            cache_mod.release_all(locks)
    except (ValueError, cache_mod.CacheError, OSError) as exc:
        return _fail(f"Falha ao importar cache: {exc}")
    print(
        f"[✓] Cache importado: {stats['imported']} novo(s), "
        f"{stats['skipped']} já existente(s), {stats['rejected']} rejeitado(s)"
    )
    return 0


def _add_cache_transport_args(parser: argparse.ArgumentParser, *, export: bool) -> None:
    parser.add_argument("project", nargs="?", default=".", help="diretório do projeto")
    if export:
        parser.add_argument("-o", "--output", required=True, help="arquivo .tar.gz de saída")
    else:
        parser.add_argument("bundle", help="arquivo .tar.gz de entrada")
        parser.add_argument("--force", action="store_true", help="aceita projectId diferente")
    parser.add_argument("--cache-dir", help="raiz do cache (sobrescreve apkfast.json)")
    parser.add_argument("--cache-mode", choices=("trusted", "verify"), default=None)
    parser.add_argument("--cache-namespace", default="")
    parser.add_argument("--require-cache-signature", action="store_true", default=None)
    parser.add_argument("--abi", default="arm64-v8a", help="ABI alvo ou all")
    parser.add_argument("--abis", default="", help="lista de ABIs separadas por vírgula")
    _add_quiet(parser)


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="apkfast",
        description="Monta, compila, otimiza e entrega APKs sem Gradle (NativeActivity first).",
    )
    parser.add_argument("--version", action="version", version=f"apkfast {__version__}")
    parser.add_argument(
        "-q", "--quiet", action="store_true", help="silencioso: mostra só erros"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_build = sub.add_parser("build", help="compila e gera o APK final")
    _add_build_args(p_build)
    p_build.set_defaults(func=_cmd_build)

    p_init = sub.add_parser("init", help="cria projeto com assistente interativo")
    p_init.add_argument("name", help="nome/diretório do projeto")
    p_init.add_argument("--force", action="store_true", help="sobrescreve diretório existente")
    p_init.add_argument(
        "--template",
        choices=("nativeapp-gl", "nativeapp-vulkan", "raylib", "sdl2", "sdl3", "bare"),
        help="template (pula a pergunta)",
    )
    p_init.add_argument("--abis", default="arm64-v8a", help="ABIs (default: arm64-v8a)")
    p_init.add_argument("--api", type=int, default=36, help="API do NDK (default: 36)")
    p_init.add_argument("--min-sdk", type=int, default=21, help="minSdk (default: 21)")
    p_init.add_argument("--target-sdk", type=int, default=36, help="targetSdk (default: 36)")
    p_init.add_argument(
        "-y", "--yes", action="store_true",
        help="sem perguntas: usa defaults (ou --template/--abis)",
    )
    _add_quiet(p_init)
    p_init.set_defaults(func=_cmd_init)

    p_doctor = sub.add_parser("doctor", help="verifica SDK, NDK e ferramentas")
    p_doctor.add_argument("--abi", default="arm64-v8a")
    p_doctor.add_argument("--api", type=int, default=36)
    _add_quiet(p_doctor)
    p_doctor.set_defaults(func=_cmd_doctor)

    p_install = sub.add_parser("install", help="instala APK via adb")
    p_install.add_argument("apk")
    p_install.add_argument("-d", "--device", help="serial do dispositivo")
    p_install.add_argument("--adb", help="caminho explícito do adb")
    p_install.add_argument(
        "--no-reinstall", action="store_true", help="não usa -r (falha se já instalado)"
    )
    p_install.add_argument(
        "-g", "--grant-permissions", action="store_true",
        help="concede automaticamente permissões perigosas (adb install -g)",
    )
    p_install.add_argument("--debug", action="store_true", help="permite downgrade (adb -d)")
    _add_quiet(p_install)
    p_install.set_defaults(func=_cmd_install)

    p_devices = sub.add_parser("devices", help="lista dispositivos adb")
    p_devices.add_argument("--adb", help="caminho explícito do adb")
    p_devices.add_argument("--restart", action="store_true", help="reinicia servidor adb antes")
    _add_quiet(p_devices)
    p_devices.set_defaults(func=_cmd_devices)

    p_setup_sdk = sub.add_parser("setup-sdk", help="instala SDK/NDK para Termux")
    p_setup_sdk.add_argument("--target", help="diretório base (default: ~/Android)")
    p_setup_sdk.add_argument("--sdk-version", help="versão do SDK (ex: 37.0.0)")
    p_setup_sdk.add_argument("--ndk-version", help="versão do NDK (ex: r30)")
    p_setup_sdk.add_argument(
        "--platform-api", type=int, default=None,
        help="API da platform via sdkmanager (default: 36, ou a API de --sdk-version)",
    )
    p_setup_sdk.add_argument("--force", action="store_true", help="reinstala se já existir")
    p_setup_sdk.add_argument("--list-versions", action="store_true", help="lista releases")
    p_setup_sdk.add_argument("-y", "--yes", action="store_true", help="sem confirmação")
    _add_quiet(p_setup_sdk)
    p_setup_sdk.set_defaults(func=_cmd_setup_sdk)

    p_setup = sub.add_parser("setup-libs", help="baixa/compila raylib + SDL2 + SDL3")
    p_setup.add_argument(
        "--abis", default="arm64-v8a",
        help="ABIs separadas por vírgula (default: arm64-v8a; use 'all' para todas)",
    )
    _add_quiet(p_setup)
    p_setup.set_defaults(func=_cmd_setup_libs)

    p_sdl = sub.add_parser("sdl-install", help="compila/instala SDL2 ou SDL3 para Android")
    p_sdl.add_argument("version", type=int, choices=(2, 3), help="2 ou 3")
    p_sdl.add_argument("--abi", default="arm64-v8a", help="ABI alvo (default: arm64-v8a)")
    p_sdl.add_argument("--api", type=int, default=21, help="API Android (default: 21)")
    _add_quiet(p_sdl)
    p_sdl.set_defaults(func=_cmd_sdl_install)

    p_ray = sub.add_parser("raylib-install", help="compila/instala raylib para Android")
    p_ray.add_argument("--abi", default="arm64-v8a", help="ABI alvo (default: arm64-v8a)")
    p_ray.add_argument("--api", type=int, default=36, help="API Android (default: 36)")
    _add_quiet(p_ray)
    p_ray.set_defaults(func=_cmd_raylib_install)

    p_cache_export = sub.add_parser("cache-export", help="exporta o cache para .tar.gz")
    _add_cache_transport_args(p_cache_export, export=True)
    p_cache_export.set_defaults(func=_cmd_cache_export)

    p_cache_import = sub.add_parser("cache-import", help="importa cache de .tar.gz")
    _add_cache_transport_args(p_cache_import, export=False)
    p_cache_import.set_defaults(func=_cmd_cache_import)

    p_luna = sub.add_parser("lunasvg-install", help="prepara LunaSVG (SVG completo)")
    _add_quiet(p_luna)
    p_luna.set_defaults(func=_cmd_lunasvg_install)

    p_webp = sub.add_parser("webp-install", help="compila libwebp (WebP animado) para Android")
    p_webp.add_argument("--abi", default="arm64-v8a", help="ABI alvo (default: arm64-v8a)")
    p_webp.add_argument("--api", type=int, default=21, help="API Android (default: 21)")
    _add_quiet(p_webp)
    p_webp.set_defaults(func=_cmd_webp_install)

    p_logs = sub.add_parser("logs", help="mostra logs/crash do dispositivo")
    p_logs.add_argument("--adb", help="caminho explícito do adb")
    p_logs.add_argument("--crash", action="store_true", help="mostra apenas buffer de crash")
    p_logs.add_argument("--clear", action="store_true", help="limpa o logcat antes")
    p_logs.add_argument("-p", "--package", help="filtra por package/tag")
    p_logs.add_argument("-n", "--lines", type=int, default=200, help="linhas (default: 200)")
    _add_quiet(p_logs)
    p_logs.set_defaults(func=_cmd_logs)

    p_info = sub.add_parser("info", help="mostra metadados do APK")
    p_info.add_argument("apk")
    _add_quiet(p_info)
    p_info.set_defaults(func=_cmd_info)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = create_parser()
    args = parser.parse_args(argv)
    args.quiet = _quiet(args)
    if args.command == "build" and args.verbose and not args.quiet:
        print(BANNER)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
