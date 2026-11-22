"""Assistente de criação de projetos apkfast (interativo ou não-interativo)."""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

TEMPLATES = ("nativeapp-gl", "nativeapp-vulkan", "raylib", "sdl2", "sdl3", "bare")
ALL_ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")

COMMON_PERMISSIONS = (
    "android.permission.INTERNET",
    "android.permission.VIBRATE",
    "android.permission.CAMERA",
    "android.permission.RECORD_AUDIO",
    "android.permission.ACCESS_FINE_LOCATION",
    "android.permission.READ_EXTERNAL_STORAGE",
    "android.permission.WRITE_EXTERNAL_STORAGE",
)


@dataclass
class ProjectConfig:
    name: str = "Meu App"
    package: str = "com.example.meuapp"
    template: str = "nativeapp-gl"
    abis: str = "arm64-v8a"
    api: int = 36
    min_sdk: int = 21
    target_sdk: int = 36
    optimize: str = "release"
    obfuscate: bool = False
    orientation: str = "unspecified"
    permissions: list[str] = field(default_factory=list)
    fullscreen: bool = True
    release: bool = False
    keystore: str = ""


def _ask(prompt: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    try:
        value = input(f"{prompt}{suffix}: ").strip()
    except EOFError:
        value = ""
    return value or default


def _ask_choice(prompt: str, choices: tuple[str, ...], default: str) -> str:
    print(f"\n{prompt}")
    for i, choice in enumerate(choices, 1):
        mark = " *" if choice == default else ""
        print(f"  {i}) {choice}{mark}")
    raw = _ask("Escolha o número (ENTER = default)", str(choices.index(default) + 1))
    try:
        index = int(raw) - 1
        if 0 <= index < len(choices):
            return choices[index]
    except ValueError:
        pass
    return default


def _ask_yes(prompt: str, default: bool = False) -> bool:
    hint = "S/n" if default else "s/N"
    raw = _ask(f"{prompt} ({hint})", "s" if default else "n").lower()
    return raw.startswith("s")


def _ask_permissions() -> list[str]:
    print("\nPermissões (digite números separados por vírgula; ENTER = nenhuma):")
    for i, permission in enumerate(COMMON_PERMISSIONS, 1):
        print(f"  {i}) {permission}")
    raw = _ask("Permissões", "")
    selected: list[str] = []
    for token in raw.split(","):
        token = token.strip()
        if not token:
            continue
        if token.isdigit() and 1 <= int(token) <= len(COMMON_PERMISSIONS):
            selected.append(COMMON_PERMISSIONS[int(token) - 1])
        elif "." in token:
            selected.append(token)
    return selected


def interactive_config(args, target: Path) -> ProjectConfig | None:
    if args.yes:
        return ProjectConfig(
            name=target.name,
            package=f"com.example.{_slug(target.name)}",
            template=args.template or "nativeapp-gl",
            abis=args.abis,
            api=args.api,
            min_sdk=args.min_sdk,
            target_sdk=args.target_sdk,
        )

    print("\n=== apkfast init ===")
    print("Deixe em branco para aceitar o valor entre [colchetes].\n")

    name = _ask("Nome do app", target.name)
    default_pkg = f"com.example.{_slug(name)}"
    package = _ask("Package (ex: com.empresa.app)", default_pkg)
    if not re.fullmatch(r"[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+", package):
        print(f"[✗] Package inválido: {package}", file=sys.stderr)
        return None

    template = args.template or _ask_choice(
        "Template de projeto:", TEMPLATES, "nativeapp-gl"
    )
    abis = _ask_choice("ABIs alvo:", ("arm64-v8a", "all", "custom"), "arm64-v8a")
    if abis == "custom":
        print("  ABIs disponíveis: " + ", ".join(ALL_ABIS))
        abis = _ask("Lista separada por vírgula", "arm64-v8a,armeabi-v7a")

    api = int(_ask("API do NDK/clang", str(args.api)))
    min_sdk = int(_ask("minSdkVersion", str(args.min_sdk)))
    target_sdk = int(_ask("targetSdkVersion", str(args.target_sdk)))
    optimize = _ask_choice(
        "Otimização:", ("release", "size", "none"), "release"
    )
    obfuscate = _ask_yes("Ofuscar (strip + R8)?", False)
    orientation = _ask_choice(
        "Orientação:", ("unspecified", "landscape", "portrait"), "unspecified"
    )
    fullscreen = _ask_yes("Tela cheia (sem status bar)?", True)
    permissions = _ask_permissions()
    release = _ask_yes("Configurar para release (keystore próprio)?", False)
    keystore = _ask("Caminho do keystore", "") if release else ""

    return ProjectConfig(
        name=name,
        package=package,
        template=template,
        abis=abis,
        api=api,
        min_sdk=min_sdk,
        target_sdk=target_sdk,
        optimize=optimize,
        obfuscate=obfuscate,
        orientation=orientation,
        permissions=permissions,
        fullscreen=fullscreen,
        release=release,
        keystore=keystore,
    )


def _slug(name: str) -> str:
    slug = re.sub(r"[^a-zA-Z0-9]+", "", name.lower())
    return slug or "app"


def render_manifest(config: ProjectConfig) -> str:
    uses_feature = ""
    if config.template in ("nativeapp-gl", "raylib", "sdl2", "sdl3"):
        uses_feature = '    <uses-feature android:glEsVersion="0x00020000" android:required="true" />\n'
    elif config.template == "nativeapp-vulkan":
        uses_feature = (
            '    <uses-feature android:name="android.hardware.vulkan.version" '
            'android:required="true" />\n'
        )

    permissions = "".join(
        f'    <uses-permission android:name="{p}" />\n' for p in config.permissions
    )

    activity_name = (
        "org.libsdl.app.SDLActivity"
        if config.template in ("sdl2", "sdl3")
        else "android.app.NativeActivity"
    )
    has_code = "" if config.template in ("sdl2", "sdl3") else ' android:hasCode="false"'
    theme = ""
    if config.fullscreen:
        theme = ' android:theme="@android:style/Theme.NoTitleBar.Fullscreen"'
    orientation = (
        f' android:screenOrientation="{config.orientation}"'
        if config.orientation != "unspecified"
        else ""
    )

    extra_meta = ""
    if config.template in ("sdl2", "sdl3"):
        extra_meta = (
            '            <meta-data android:name="android.app.lib_name" '
            'android:value="main" />\n'
        )

    uses_sdk = ""
    if config.min_sdk != 21 or config.target_sdk != 36:
        uses_sdk = (
            f'    <uses-sdk android:minSdkVersion="{config.min_sdk}" '
            f'android:targetSdkVersion="{config.target_sdk}" />\n'
        )

    return f"""<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="{config.package}">

{uses_sdk}{uses_feature}{permissions}
    <application android:label="{config.name}"{has_code}{theme}>
        <activity android:name="{activity_name}"
                  android:label="{config.name}"
                  android:exported="true"{orientation}
                  android:configChanges="orientation|keyboardHidden|screenSize">
            {extra_meta}            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
"""


def render_source(config: ProjectConfig) -> str:
    if config.template == "bare":
        return """#include <android_native_app_glue.h>

void android_main(struct android_app* app) {
    while (true) {
        int events;
        android_poll_source* source;
        while (ALooper_pollOnce(0, nullptr, &events, (void**)&source) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) return;
        }
    }
}
"""
    if config.template == "nativeapp-gl":
        return """#include <nativeapp/nativeapp.hpp>

using namespace nativeapp;

class MeuApp : public App {
public:
    explicit MeuApp(android_app* app) : App(app) {}
private:
    GLES2Scene scene_;
    void on_ready() override {}
    void on_surface_created() override { scene_.init(window()); }
    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
    void on_draw() override { scene_.render(); }
};

APP_MAIN(MeuApp)
"""
    if config.template == "nativeapp-vulkan":
        return """#include <nativeapp/nativeapp.hpp>

using namespace nativeapp;

class MeuApp : public App {
public:
    explicit MeuApp(android_app* app) : App(app) {}
private:
    VulkanScene scene_;
    void on_ready() override { scene_.init(activity()); }
    void on_surface_created() override { scene_.attach(window()); }
    void on_surface_destroyed() override { scene_.invalidate(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
    void on_draw() override { scene_.render(); }
};

APP_MAIN(MeuApp)
"""
    if config.template == "raylib":
        return """#include <raylib.h>

int main(void) {
    InitWindow(0, 0, "raylib");
    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        BeginDrawing();
          ClearBackground(DARKBLUE);
          DrawText("raylib + apkfast!", 40, 80, 32, WHITE);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}
"""
    if config.template in ("sdl2", "sdl3"):
        include = "<SDL.h>" if config.template == "sdl2" else "<SDL3/SDL.h>"
        main_inc = "<SDL_main.h>" if config.template == "sdl2" else "<SDL3/SDL_main.h>"
        init = (
            'SDL_Init(SDL_INIT_VIDEO) < 0'
            if config.template == "sdl2"
            else '!SDL_Init(SDL_INIT_VIDEO)'
        )
        create = (
            'SDL_CreateWindow("SDL", 0, 0, 0, 0, SDL_WINDOW_SHOWN)'
            if config.template == "sdl2"
            else 'SDL_CreateWindow("SDL", 0, 0, 0)'
        )
        quit_event = "SDL_QUIT" if config.template == "sdl2" else "SDL_EVENT_QUIT"
        color = "SDL_SetRenderDrawColor(renderer, 0, 82, 172, 255);"
        create_renderer = (
            "SDL_CreateRenderer(window, -1, 0)"
            if config.template == "sdl2"
            else "SDL_CreateRenderer(window, nullptr)"
        )
        return f"""#include {include}
#include {main_inc}

// SDLActivity (Java) chama SDL_main diretamente; não precisa de android_main.
int main(int argc, char* argv[]) {{
    (void)argc; (void)argv;
    if ({init}) return 1;
    SDL_Window* window = {create};
    SDL_Renderer* renderer = {create_renderer};
    bool running = true;
    SDL_Event event;
    while (running) {{
        while (SDL_PollEvent(&event)) {{
            if (event.type == {quit_event}) running = false;
        }}
        {color}
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }}
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}}
"""
    return ""


def render_sdl_activity(config: ProjectConfig) -> str:
    java_dir = _slug(config.package.split(".")[-1])
    return f"""package {config.package};

import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity {{
    @Override
    protected String[] getLibraries() {{
        return new String[] {{ "main" }};
    }}
}}
"""


def create_project(target: Path, config: ProjectConfig) -> None:
    (target / "jni").mkdir(parents=True, exist_ok=True)
    (target / "assets").mkdir(parents=True, exist_ok=True)
    (target / "src" / "main" / "cpp").mkdir(parents=True, exist_ok=True)

    (target / "AndroidManifest.xml").write_text(render_manifest(config), encoding="utf-8")
    (target / "jni" / "main.cpp").write_text(render_source(config), encoding="utf-8")
    (target / "apkfast.json").write_text(
        json.dumps(
            {
                "nativeLibraryName": "main",
                "abi": config.abis,
                "api": config.api,
                "minSdk": config.min_sdk,
                "targetSdk": config.target_sdk,
                "optimize": config.optimize,
                "obfuscate": config.obfuscate,
                "release": config.release,
                "keystore": config.keystore,
                "template": config.template,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    (target / "README.md").write_text(
        f"# {config.name}\n\n"
        f"Projeto criado pelo apkfast (`{config.template}`).\n\n"
        f"```sh\npython3 apkfast.py build .\n"
        f"python3 apkfast.py install dist/*.apk\n```\n",
        encoding="utf-8",
    )

    if config.template in ("sdl2", "sdl3"):
        java_root = target / "java" / Path(*config.package.split("."))
        java_root.mkdir(parents=True, exist_ok=True)
        (java_root / "MainActivity.java").write_text(
            render_sdl_activity(config), encoding="utf-8"
        )

        # SDLActivity e derivadas vivem no sdk do SDL e são necessárias para
        # compilar o MainActivity. Copia-las para o projeto (nos exemplos isso
        # era feito manualmente).
        sdl_version = 2 if config.template == "sdl2" else 3
        sdl_java = (
            Path(__file__).resolve().parent.parent
            / "vendor"
            / f"sdl{sdl_version}"
            / "src"
            / "android-project"
            / "app"
            / "src"
            / "main"
            / "java"
            / "org"
            / "libsdl"
            / "app"
        )
        if sdl_java.is_dir():
            dest = target / "java" / "org" / "libsdl" / "app"
            dest.mkdir(parents=True, exist_ok=True)
            for source in sorted(sdl_java.glob("*.java")):
                (dest / source.name).write_text(
                    source.read_text(encoding="utf-8", errors="ignore"),
                    encoding="utf-8",
                )

    if config.template == "raylib" and config.abis == "all":
        (target / "NOTES.md").write_text(
            "raylib é linkada estaticamente; rode `apkfast setup-libs --abis all`.\n",
            encoding="utf-8",
        )
