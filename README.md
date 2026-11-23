# apkfast

[![CI](https://github.com/danieldossilvareis-sudo/apkfast/actions/workflows/ci.yml/badge.svg)](https://github.com/danieldossilvareis-sudo/apkfast/actions/workflows/ci.yml)
[![Version](https://img.shields.io/badge/version-2.4.1-blue.svg)](CHANGELOG.md)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

CLI para **montar, compilar, otimizar e entregar APKs sem Gradle**, com suporte
dedicado a apps **sem Java** (`NativeActivity`).

Projetado para Termux/arm64, mas detecta automaticamente SDK/NDK e escolhe
ferramentas compatíveis com o host (ignora binários x86_64 que não executam no
dispositivo).

## Requisitos detectados automaticamente

- Android SDK em `~/Android/android-sdk`
- Android NDK em `~/Android/android-ndk-*`
- `aapt2`, `zipalign`, `apksigner`, `clang` do NDK, `java`, `keytool`
- `cmake` + `git` (somente para `setup-libs`)

Você pode sobrescrever com `--sdk-dir`, `--ndk-dir`, `--build-tools` ou
`APKFAST_ANDROID_ROOT`.

## Instalação

### Via PyPI (recomendado)

```sh
python3 -m pip install apkfast
apkfast --version
apkfast doctor
```

O wheel instala o comando `apkfast` e os dados de build (`include/`, `src/`,
`patches/`, `docs/`) junto com o pacote Python. O SDK/NDK continua sendo
detectado em `~/Android` (ou via `--sdk-dir`/`--ndk-dir`).

### Via repositório (alternativa / desenvolvimento)

```sh
git clone https://github.com/apkfast/apkfast.git
cd apkfast
chmod +x apkfast.py
python3 -m pip install -e .          # opcional: instala o comando `apkfast`

# (Opcional) Instala SDK + NDK pré-compilados para Termux
python3 apkfast.py setup-sdk              # ~340 MB em ~/Android
python3 apkfast.py setup-sdk --list-versions

# Verifica o ambiente
python3 apkfast.py doctor

# (Opcional) Bibliotecas avançadas: raylib + SDL2 + SDL3 + libwebp + LunaSVG
python3 apkfast.py setup-libs --abis all       # ou --abis arm64-v8a

# Link global sem pip (Termux)
ln -s "$PWD/apkfast.py" "$PREFIX/bin/apkfast"
```

> O repositório **não versiona** bibliotecas binárias. `setup-libs` baixa e
> compila raylib/SDL2/SDL3, libwebp e LunaSVG para as ABIs escolhidas usando o
> seu NDK.
>
> `setup-sdk` instala SDK/NDK pré-compilados para `aarch64-linux-android`
> (Termux) a partir dos releases de
> [HomuHomu833/android-sdk-custom](https://github.com/HomuHomu833/android-sdk-custom)
> e [HomuHomu833/android-ndk-custom](https://github.com/HomuHomu833/android-ndk-custom).
> Se você já tem SDK/NDK em `~/Android`, o comando detecta e não baixa nada
> (use `--force` para reinstalar).

## Estabilidade e compatibilidade

- **API versionada:** `nativeapp/` segue SemVer; a versão contratual está em
  `include/nativeapp/version.hpp` (`APKFAST_API_VERSION*`) e é exibida por
  `apkfast doctor --json`. Headers são **source-compatible**, não ABI —
  recompile o app ao atualizar. Política completa:
  [docs/compatibility.md](docs/compatibility.md).
- **Toolchain reprodutível:** `apkfast build . --generate-lock` cria
  `apkfast.lock.json` (lockVersion 1) com NDK, build-tools, API e checksums.
  Builds divergentes falham; `--ignore-lock` transforma em aviso.
- **Config validada:** chaves/tipos de `apkfast.json` são checados com mensagens
  claras. Inspecione com `apkfast config show .`.
- **Matriz de build no CI:** API 21/28/35 × x86_64 e API 35 × arm64-v8a/
  armeabi-v7a compilam `native-hello`, `image-demo` e `permissions-test` com
  lockfile validado. GitHub-hosted runners não bootam emuladores ARM de forma
  confiável; a validação em dispositivo é feita localmente com `apkfast
  install` + `apkfast logs`.
- **Migração:** veja [docs/migration.md](docs/migration.md) (inclui o aviso de
  que o cache schema 1 da v2.2 foi invalidado na v2.3).

## Novo modelo de ciclo de vida (nativeapp)

O exemplo abaixo usa `on_surface_created`/`on_surface_destroyed` (recomendado):

```cpp
#include <nativeapp/nativeapp.hpp>

class App : public nativeapp::App {
    nativeapp::GLES2Scene scene_;
    void on_ready() override {}
    void on_surface_created() override { scene_.init(window()); }
    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
    void on_draw() override { scene_.render(); }
};
APP_MAIN(App)
```

## Assistente `init`

`apkfast init` faz perguntas e gera um projeto completo:

```sh
python3 apkfast.py init meu-app
```

Perguntas: nome, package, template (`nativeapp-gl`, `nativeapp-vulkan`,
`raylib`, `sdl2`, `sdl3`, `bare`), ABIs, API/minSdk/targetSdk, otimização,
ofuscação, orientação, tela cheia, permissões (multi-select) e keystore release.

O projeto gerado já vem com `AndroidManifest.xml`, `jni/main.cpp`,
`apkfast.json` (com todas as escolhas), `README.md`, e — para SDL — a
`MainActivity.java` com `getLibraries()` correto.

Modo não-interativo:

```sh
python3 apkfast.py init meu-app -y --template nativeapp-vulkan --abis all
python3 apkfast.py init jogo -y --template sdl3 --abis arm64-v8a,armeabi-v7a
```

## API nativeapp (novo)

Para não precisar escrever EGL/Vulkan/JNI na mão, use a API header-only em
`include/nativeapp/`:

```cpp
#include <nativeapp/nativeapp.hpp>

class App : public nativeapp::App {
    nativeapp::GLES2Scene scene_;
    void on_ready() override { scene_.init(window()); set_ready(); }
    void on_resize(int w, int h) override { scene_.resize(w, h); }
    void on_draw() override { scene_.render(); }
};
APP_MAIN(App)
```

- `nativeapp::GLES2Scene` — cena OpenGL ES 2 pronta (shader, VBO, toque)
- `nativeapp::VulkanScene` — swapchain + clear/present Vulkan
- `nativeapp::Permissions` — permissões em runtime via JNI (API 23+)
- `nativeapp::App` — máquina de estados (`Created → Ready ↔ Paused → Destroyed`)
  com `on_surface_created()` / `on_surface_destroyed()` para recriar EGL/swapchain
  ao voltar de background (evita tela preta)

Documentação completa:
- [Guias individuais de cada helper](docs/README.md) — recomendado
  (inclui [bibliotecas próprias](docs/libraries.md),
  [WebP animado/SVG completo](docs/image-formats.md),
  [build incremental](docs/incremental.md) e
  [cache remoto](docs/cache-remote.md))
- [Referência consolidada da API](include/nativeapp/README.md)
Exemplos: `examples/nativeapp-gl`, `examples/nativeapp-vulkan`,
`examples/nativeapp-advanced` (Mesh/Material/matrizes/dt/Scene) e
`examples/permissions-test` (teste real de permissões com callback).

A API agora inclui `Shader`, `Mesh`, `Texture`, `GLContext`, `GraphicsPipeline`
Vulkan com depth + 2 frames in-flight, matemática `Mat4`, `on_update(dt)`,
input polling, assets via `AAssetManager`, **`TextInput`** (teclado virtual
via JNI, sem EditText), **áudio** (AAudio + OpenSLES), **imagens** PNG/JPEG,
**WebP estático e animado** (libwebp), **SVG completo** (LunaSVG: CSS, texto,
clip/mask/pattern/filtros), **sensores**, **gamepad/mouse** e **device**
(vibração/bateria/orientação). Exemplos: `examples/text-input-hello`,
`examples/device-demo` e `examples/image-demo`. Veja a
[documentação completa](include/nativeapp/README.md).

A API de permissões também funciona com **raylib** (`GetAndroidApp()`) e
**SDL2/SDL3** (`JniPermissions` via `SDL_AndroidGetActivity()` /
`SDL_GetAndroidActivity()`). Exemplos: `examples/raylib-permissions` e
`examples/sdl3-permissions`.

## Uso rápido

```sh
# Verifica o ambiente
python3 apkfast.py doctor

# Cria projeto com assistente interativo (nome, pacote, template, ABIs,
# permissões, orientação, otimização, ofuscação, keystore...)
python3 apkfast.py init meu-app

# Modo não-interativo (para scripts/CI)
python3 apkfast.py init meu-app -y --template sdl3 --abis all

# Compila e gera APK assinado
cd examples/native-hello
python3 ../../apkfast.py build -o dist/native-hello.apk

# Instala no dispositivo (detecta dispositivo online, reinicia adb se offline)
python3 ../../apkfast.py install dist/native-hello.apk

# Lista dispositivos / concede permissões automaticamente
python3 ../../apkfast.py devices
python3 ../../apkfast.py install dist/native-hello.apk -g

# Inspeciona o APK
python3 ../../apkfast.py info dist/native-hello.apk

# Cache remoto/compartilhado
python3 ../../apkfast.py cache-export . -o cache.tar.gz --abi all
python3 ../../apkfast.py cache-import . cache.tar.gz --cache-dir /mnt/nfs/apkfast
python3 ../../apkfast.py cache-push s3://bucket/apkfast/cache.tar.gz .
python3 ../../apkfast.py cache-pull https://server/cache.tar.gz .
```

## Estrutura esperada do projeto

```text
meu-app/
├── AndroidManifest.xml      # obrigatório
├── jni/
│   └── main.cpp             # uma ou mais fontes .c/.cpp/.cc
├── res/                     # opcional
├── assets/                  # opcional
└── apkfast.json             # opcional (nativeLibraryName)
```

O nome da biblioteca nativa é lido de `android.app.lib_name` no manifesto,
de `apkfast.json` (`nativeLibraryName`) ou usa `main` como padrão.

## Opções principais

| Flag | Descrição |
|------|-----------|
| `--abi` | `arm64-v8a` (padrão), `armeabi-v7a`, `x86`, `x86_64` ou `all` (fat APK) |
| `--abis a,b` | lista explícita de ABIs (ex.: `arm64-v8a,armeabi-v7a`) |
| `--api` | API usada pelo clang do NDK (padrão 36) |
| `--min-sdk` / `--target-sdk` | injetados no `aapt2 link` |
| `--optimize` | `none`, `release` (padrão), `size` |
| `--release` | exige `--keystore`; não usa keystore debug |
| `--obfuscate` | `-fvisibility=hidden`, strip total, `--exclude-libs,ALL`, libc++ estático e R8 para Java |
| `-q` / `--quiet` | silencioso: imprime apenas erros |
| `-v` / `--verbose` | mostra comandos executados |
| `--keystore` / `--alias` / `--store-pass` / `--key-pass` | assinatura customizada |
| `--src` | fonte específica (repitível) |
| `--cflags` / `--ldflags` | flags extras |
| `--assets` | diretório de assets |
| `--no-incremental` | desliga o cache incremental (padrão: ligado) |
| `--clean-cache` | apaga o cache incremental antes do build |
| `--cache-dir` / `--cache-mode` | raiz compartilhada e modo `trusted`/`verify` |
| `--cache-namespace` | isola projetos com o mesmo package |
| `--require-cache-signature` | exige HMAC em cache importado |
| `--lib` / `--libs-dir` | bibliotecas `.a`/`.so` próprias (repitível) |

O keystore de debug é criado automaticamente em `~/.apkfast/debug.jks`.

### Exemplos

```sh
# Fat APK com todas as ABIs, silencioso
python3 apkfast.py build . --abi all -q

# APK release assinado e ofuscado
python3 apkfast.py build . --release --obfuscate \
  --keystore ~/meu.jks --store-pass "senha" --alias minha-chave

# Ver erro sem ruído (quiet continua mostrando falhas)
python3 apkfast.py build projeto-inexistente -q
```

> Nota: `--obfuscate` em apps puramente nativos reduz drasticamente a superfície
> de engenharia reversa (símbolos, RTTI e tabelas dinâmicas removidos). Para
> código Java, o R8 é usado quando disponível no SDK; arquivos
> `proguard-rules.pro` no projeto são respeitados.

## SDL2 e SDL3 (sem Gradle)

O apkfast inclui SDL 2.32.6 e SDL 3.5 pré-compiladas para as 4 ABIs em
`vendor/sdl2/` e `vendor/sdl3/`.

### SDL2 — com SDLActivity (Java)

Tanto SDL2 quanto SDL3 exigem a classe Java `org.libsdl.app.SDLActivity` no
Android. O apkfast a inclui automaticamente nos exemplos. Como a lib é estática,
crie uma subclasse `MainActivity` com `getLibraries()` retornando só `"main"`.

```cpp
#include <SDL.h>
#include <SDL_main.h>
#include <android_native_app_glue.h>

extern "C" void android_main(android_app* app) { (void)app; SDL_main(0, nullptr); }
int main(int argc, char* argv[]) { /* código SDL puro */ }
```

Exemplo completo: `examples/sdl2-hello`.

### SDL3 — usa SDLActivity (Java)

O SDL3 exige a classe Java `org.libsdl.app.SDLActivity` (incluída pelo apkfast
no exemplo). Como linkamos estaticamente, crie uma subclasse sobrescrevendo
`getLibraries()` para retornar apenas `"main"`:

```java
public class MainActivity extends SDLActivity {
    @Override protected String[] getLibraries() { return new String[] { "main" }; }
}
```

E no C++ defina `android_main()` chamando `SDL_main()`. Exemplo completo:
`examples/sdl3-hello`.

Para recompilar uma ABI: `python3 apkfast.py sdl-install 2 --abi <abi>` (ou 3).

## raylib (sem Gradle)

O apkfast já inclui a raylib 6.5 compilada para **todas as 4 ABIs** em
`vendor/raylib/lib/`. O comando `--abi all` funciona de imediato:

```sh
python3 apkfast.py build . --abi all
```

Para recompilar uma ABI específica (ou uma nova versão):

```sh
python3 apkfast.py raylib-install --abi armeabi-v7a
```

Para usar, basta `#include <raylib.h>` e escrever `int main()` — a raylib
fornece `android_main()` e chama seu `main()` internamente:

```cpp
#include <raylib.h>
int main(void) {
    InitWindow(0, 0, "App");
    while (!WindowShouldClose()) {
        BeginDrawing();
          ClearBackground(DARKBLUE);
          DrawText("oi", 40, 80, 32, WHITE);
        EndDrawing();
    }
    CloseWindow();
}
```

O apkfast detecta o include e adiciona automaticamente:
- `-I vendor/raylib/include`
- `libraylib-<abi>.a` + `-lGLESv2 -lEGL -landroid -llog -lz -lm`
- `-Wl,--wrap=fopen` (exigido pelo loader Android da raylib)
- glue do NDK (para `ANativeActivity_onCreate`)

Exemplo completo: `examples/raylib-hello`.

## Instalação e adb (`offline` / `unauthorized`)

O comando `install` escolhe automaticamente um dispositivo **online** e, se o
adb estiver travado, reinicia o servidor (`kill-server` + `start-server`) e
tenta novamente. Mensagens são claras:

```sh
python3 apkfast.py devices              # vê serial, estado e modelo
python3 apkfast.py devices --restart    # força restart do servidor adb
python3 apkfast.py install app.apk -d <serial>   # dispositivo específico
python3 apkfast.py install app.apk -g            # permission grant (-g)
python3 apkfast.py install app.apk --adb /caminho/adb
python3 apkfast.py install app.apk -q            # só erros
```

Se aparecer `unauthorized`, aceite a solicitação **Permitir depuração USB?**
no aparelho. Se aparecer `offline`, reconecte o USB ou rode `adb reconnect`.

## Solução de problemas: app fecha ao abrir

Use `adb logcat -b crash` para ver a causa. Dois problemas reais foram
encontrados e corrigidos pelo `apkfast`:

1. **`UnsatisfiedLinkError: dlopen failed: cannot locate symbol
   __android_log_print`** — o linker não incluía `liblog.so`. O builder agora
   adiciona automaticamente `-llog`, `-lEGL` e `-lGLESv2` quando o código usa
   `GLES2Scene`/EGL. Verifique com:
   ```sh
   llvm-readelf -d libmain.so | grep NEEDED
   # deve listar liblog.so, libEGL.so, libGLESv2.so
   ```

2. **Crash JNI (`stack overflow` em `CallIntMethod`) ao pedir permissão** —
   `android_main()` roda numa thread separada, onde `activity_->env` é
   inválido. Agora `Permissions::env()` usa `JavaVM::GetEnv` /
   `AttachCurrentThread` antes de qualquer chamada JNI.

Outras causas comuns:
- `android.app.lib_name` no manifesto diferente do `lib<name>.so` gerado
  (padrão: `main` → `libmain.so`)
- esquecer `android:exported="true"` na Activity (obrigatório na API 31+)
- ABI ausente no APK para o dispositivo (use `--abi all`)
- exceção C++ sem catch em `on_ready()` (o NDK não propaga exceções)

## Tela preta no OpenGL/Vulkan (resolvido)

Três causas foram identificadas e corrigidas na API `nativeapp`:

1. **`ALooper_pollOnce(-1, ...)` travava o loop antes da janela existir.** O
   `App::run()` agora usa `timeout=0` sempre e limita a 8 eventos por iteração,
   garantindo que `on_draw()` rode mesmo após diálogos de permissão, teclado
   ou recriação de janela.
2. **Janela recriada sem `APP_CMD_INIT_WINDOW`.** `activate_window()` é
   rechamado em `APP_CMD_RESUME` e `APP_CMD_GAINED_FOCUS` quando
   `app_->window` mudou.
3. **`-flto` + `-lvulkan` (NDK r30) removia o `DT_NEEDED libvulkan.so`.** O
   linker com LTO e `--as-needed` deixava os símbolos `vk*` como `UND` sem
   biblioteca, e o `dlopen` falhava com `cannot locate symbol vkCreateInstance`.
   Corrigido com `-Wl,--no-as-needed -lvulkan -Wl,--as-needed` **e**
   desativando LTO quando Vulkan é detectado. O stub do NDK **não** é embutido
   no APK — o Android fornece a implementação real em runtime.

Confira sempre com:
```sh
llvm-readelf -d libmain.so | grep libvulkan.so   # deve aparecer
```

Validação com screenshot via `adb exec-out screencap -p`: o exemplo GL mostra
fundo `(20,20,31)` e triângulo colorido no centro. Use `set_clear_color()` para
trocar a cor de fundo. Para depurar erros GL/EGL, a API emite warnings
`glGetError`/`eglGetError` com a tag `nativeapp-gl`.

## Pipeline interno

1. `aapt2 compile` -> `.flat` files
2. clang do NDK -> objetos + `native_app_glue` -> `lib<name>.so`
3. `aapt2 link` -> APK base
4. insere `lib/<abi>/*.so`
5. `zipalign -f 4`
6. `apksigner sign` (v2; v1 quando minSdk < 24) + `apksigner verify`
