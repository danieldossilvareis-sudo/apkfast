"""Command-line interface for apkfast."""

from __future__ import annotations

import argparse
import os
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


def _say(args: argparse.Namespace, message: str) -> None:
    if not getattr(args, "quiet", False):
        print(message)


def _add_build_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("project", nargs="?", default=".", help="diretório do projeto")
    parser.add_argument("-o", "--output", help="caminho do APK final")
    parser.add_argument("--build-dir", help="diretório de arquivos temporários")
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
    parser.add_argument("--assets", help="diretório de assets (default: <projeto>/assets)")
    parser.add_argument("-v", "--verbose", action="store_true")
    parser.add_argument(
        "-q", "--quiet", dest="build_quiet", action="store_true", help="silencioso: mostra só erros"
    )


def _cmd_build(args: argparse.Namespace) -> int:
    from .builder import Logger

    log = Logger(quiet=args.quiet or args.build_quiet, verbose=args.verbose)
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
        assets_dir=Path(args.assets) if args.assets else None,
        verbose=args.verbose,
        quiet=args.quiet or args.build_quiet,
        log=log,
    )
    try:
        apk = build(options)
    except (BuildError, RuntimeError) as exc:
        log.error(f"[✗] {exc}")
        return 1
    log.info(f"[✓] Build concluído: {apk}")
    return 0


def _cmd_init(args: argparse.Namespace) -> int:
    from .scaffold import create_project, interactive_config

    target = Path(args.name).expanduser().resolve()
    if target.exists() and any(target.iterdir()) and not args.force:
        print(f"[✗] Diretório não vazio: {target} (use --force)", file=sys.stderr)
        return 1

    config = interactive_config(args, target)
    if config is None:
        return 1

    create_project(target, config)
    _say(args, f"[✓] Projeto criado em {target}")
    _say(args, f"    template: {config.template} | package: {config.package}")
    cli_relative = os.path.relpath(Path(__file__).resolve().parent.parent, target)
    _say(args, f"    cd {target} && python3 {cli_relative}/apkfast.py build")
    if config.template in ("raylib", "sdl2", "sdl3"):
        _say(args, "    (instale as libs: python3 ../../apkfast.py setup-libs)")
    return 0


def _cmd_doctor(args: argparse.Namespace) -> int:
    _say(args, BANNER)
    try:
        tc = detect(abi=args.abi, api=args.api)
    except RuntimeError as exc:
        print(f"[✗] {exc}", file=sys.stderr)
        return 1
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
    except DeviceError as exc:
        print(f"[✗] {exc}", file=sys.stderr)
        return 1
    _say(args, f"[✓] Instalado em {args.device or 'dispositivo'}: {output}")
    return 0


def _cmd_setup_sdk(args: argparse.Namespace) -> int:
    from .sdk_setup import SetupError, install_sdk, list_versions

    if args.list_versions:
        print("SDK (android-sdk-custom):")
        for version in list_versions("HomuHomu833/android-sdk-custom"):
            print(f"  {version}")
        print("NDK (android-ndk-custom):")
        for version in list_versions("HomuHomu833/android-ndk-custom"):
            print(f"  {version}")
        return 0

    try:
        paths = install_sdk(
            target=Path(args.target) if args.target else None,
            sdk_version=args.sdk_version,
            ndk_version=args.ndk_version,
            platform_api=args.platform_api,
            force=args.force,
            quiet=args.quiet,
            assume_yes=args.yes,
        )
    except SetupError as exc:
        print(f"[✗] {exc}", file=sys.stderr)
        return 1
    _say(args, f"[✓] SDK: {paths['sdk']}")
    _say(args, f"[✓] NDK: {paths['ndk']}")
    _say(args, "Rode: python3 apkfast.py doctor")
    return 0


def _cmd_setup_libs(args: argparse.Namespace) -> int:
    """Baixa/compila raylib + SDL2 + SDL3 para as ABIs pedidas."""
    from .raylib import RaylibError, install as raylib_install
    from .sdl import SDLError, install as sdl_install

    if args.abis.strip() == "all":
        from .builder import ALL_ABIS

        abis = list(ALL_ABIS)
    else:
        abis = [a.strip() for a in args.abis.split(",") if a.strip()] or ["arm64-v8a"]
    for abi in abis:
        for name, fn in (
            ("raylib", lambda: raylib_install(abi, api=36, quiet=args.quiet)),
            ("SDL2", lambda: sdl_install(2, abi, api=21, quiet=args.quiet)),
            ("SDL3", lambda: sdl_install(3, abi, api=21, quiet=args.quiet)),
        ):
            try:
                fn()
            except (RaylibError, SDLError) as exc:
                print(f"[✗] {name} ({abi}): {exc}", file=sys.stderr)
                return 1
    _say(args, "[✓] Bibliotecas prontas.")
    return 0


def _cmd_sdl_install(args: argparse.Namespace) -> int:
    from .sdl import SDLError, install

    try:
        install(args.version, abi=args.abi, api=args.api, quiet=args.quiet)
    except SDLError as exc:
        print(f"[✗] {exc}", file=sys.stderr)
        return 1
    return 0


def _cmd_raylib_install(args: argparse.Namespace) -> int:
    from .raylib import RaylibError, install

    try:
        install(abi=args.abi, api=args.api, quiet=args.quiet)
    except RaylibError as exc:
        print(f"[✗] {exc}", file=sys.stderr)
        return 1
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
    except DeviceError as exc:
        print(f"[✗] {exc}", file=sys.stderr)
        return 1


def _cmd_devices(args: argparse.Namespace) -> int:
    from .device import DeviceError, find_adb, list_devices, restart_server

    try:
        adb = find_adb(args.adb)
        devices = list_devices(adb)
        if not devices and args.restart:
            restart_server(adb)
            devices = list_devices(adb)
    except DeviceError as exc:
        print(f"[✗] {exc}", file=sys.stderr)
        return 1

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
        print(f"[✗] APK não encontrado: {path}", file=sys.stderr)
        return 1
    tc = detect()
    result = subprocess.run(
        [str(tc.aapt2), "dump", "badging", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    print(result.stdout or result.stderr)
    return result.returncode


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
        "--platform-api", type=int, default=36,
        help="API da platform a instalar via sdkmanager (default: 36)",
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
    if getattr(args, "cmd_quiet", False):
        args.quiet = True
    if getattr(args, "build_quiet", False):
        args.quiet = True
    if args.command == "build" and args.verbose and not args.quiet:
        print(BANNER)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
