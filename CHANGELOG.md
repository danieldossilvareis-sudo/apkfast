# Changelog

## [2.1.0] - 2026-11-23 — Bibliotecas próprias (.a/.so)

### Adicionado
- **Suporte a bibliotecas já compiladas**: convenção `libs/<abi>/*.a` e
  `*.so`, flags `--lib` (repitível) e `--libs-dir`, e chaves `libraries` /
  `libsDir` em `apkfast.json`
- `.a` é linkada dentro de `libmain.so` (antes das libs do sistema); `.so` é
  copiada para `lib/<abi>/` no APK
- Isolamento por ABI em fat APK (uma lib arm64 nunca é linkada em x86)
- scaffolds criam as pastas `libs/<abi>/`
- Guia `docs/libraries.md`

### Validado
- lib estática própria retornando 42: `mylib-test: answer=42` no dispositivo
- `.so` externa presente em `lib/arm64-v8a/` no APK
- `--abi all` com libs apenas arm64 — sem contaminação entre ABIs
- erro claro para `--lib` inexistente

## [2.0.2] - 2026-11-23 — Auditoria completa (6 equipes paralelas)

Seis equipes independentes auditaram e corrigiram todo o código-fonte em
escopos exclusivos: build pipeline, CLI/setup, device/manifest, headers core,
gráficos e subsistemas. Documentação sincronizada com o código resultante.

### Correções de maior impacto
- **Build**: `-q` agora é realmente silencioso (captura stdout/stderr de todos
  os subprocessos), lock de build contra corrida, detecção de features ignora
  comentários/strings, `--output`/`--build-dir` não podem apagar o projeto,
  `_version_sort_key` ordena `r30 > r9` e `android-36 > android-9`, host-tag
  não escolhe mais toolchain darwin por acidente
- **CLI/setup**: exceções tratadas sem traceback, ABIs/inteiros validados no
  wizard, templates SDL2/SDL3 corrigidos, `setup-sdk` com normalização de
  versões, licenças, rollback com backup e NDK dir derivado da release
- **Gráficos**: `GLContext` idempotente/transacional, geração de contexto
  evita reutilizar nomes GL de contexto morto, `draw_text()` com VBO
  persistente (antes alocava por frame), pipeline Vulkan transacional com
  dynamic viewport, deadlock de fence corrigido, `GpuBuffer` Checado
- **Áudio/mídia**: `set_volume()` efetivo, resample mono/22050→44100/estéreo,
  `Codecs` com `unique_ptr`/overflow guards, `Music` thread-safe com
  resample clientside e loop iterativo
- **Ciclo de vida**: `finish()` idempotente, reset de input em
  TERM_WINDOW/PAUSE/DESTROY, multi-toque POINTER_UP/CANCEL correto,
  guards de NaN/inf em matrizes, `AAsset` com leitura em laço
- **Raylib local**: `vendor/raylib/include/raylib.h` e a `.a` arm64 estavam
  corrompidos por builds concorrentes — restaurados

### Validação
- 13 exemplos compilam
- 6 apps rodando no dispositivo (GL, Vulkan, permissões, teclado, device,
  media) com 0 crash
- 124 links de docs verificados, nenhum quebrado
- CI verde

## [2.0.1] - 2026-11-23 — Correção de renderização de texto

### Corrigido
- **Texto não aparecia no media-demo**: o atlas de fonte era uploaded como
  `GL_ALPHA` (problemático em alguns drivers GLES3) e os UVs eram invertidos
  no shader. Agora o atlas é expandido para RGBA e os UVs do
  `stbtt_GetPackedQuad(..., 1)` são usados diretamente
- `draw_text()` usa o location de atributo correto (2 = `a_uv`, fixo pelo
  `Shader::link()`) e desabilita depth test para texto 2D
- Caminhos de assets documentados como relativos (`"font.ttf"`)

## [2.0.0] - 2026-11-23 — Backend completo multimídia

### Adicionado
- **Fontes TTF/OTF** via stb_truetype: `FontAtlas` + `GLES2Scene::load_font()`
  e `draw_text()` (atlas ASCII 32..126, cores por chamada)
- **OGG Vorbis** via stb_vorbis e **MP3** via dr_mp3 — `Codecs::decode_*()`
  e `Audio::load()` com detecção por extensão (.wav/.ogg/.mp3)
- **`Music`** — streaming de música com AAudio data callback (API 26+),
  loop, volume, resample 44100 Hz, memória constante
- **Injeção automática do TU third-party** (`src/nativeapp_impl.cpp`) pelo
  builder: uma única compilação de stb_image, stb_truetype, stb_vorbis e
  dr_mp3 por app — corrige o problema de `*_IMPLEMENTATION` dentro de headers
- Exemplo `examples/media-demo` com TTF, PNG, WAV, OGG, MP3 e streaming
- Guias `docs/fonts.md` e `docs/audio-codecs.md`

### Compatibilidade
- Caminhos de assets são **relativos** ao diretório `assets/` do projeto
  (`"tone.ogg"`, não `"assets/tone.ogg"`) — documentado
- WebP e SVG ficam para v2.1 (decisão de escopo)

### Validado
- `media-demo` no dispositivo: sprite 64×64, fonte carregada, WAV 22050
  samples, OGG/MP3 176400 samples, música em stream aberta, 0 crash

## [1.0.0] - 2026-11-22 (pós-release)

### Documentação
- `docs/` com 12 guias individuais e ricos (8.900+ linhas), um por helper:
  App, OpenGL, Vulkan, permissões, text input, áudio, sensores, device,
  input helpers, assets, matemática e tipos gráficos — todos baseados no
  código real, com tabelas de API, exemplos compiláveis e armadilhas
- Índice em `docs/README.md`, linkado no README principal

### Correções descobertas durante a documentação
- `MeshData::add_triangle()` agora rejeita malhas acima de 65536 vértices
  (protege o índice `uint16_t`)
- `GLES2Scene::draw_mesh()` não dereferencia mais `end()` quando uma textura
  falha a carregar
- `Audio::play_opensles()` aplica volume (antes ignorava) e mantém o buffer
  vivo durante o `Enqueue` (evita áudio corrompido)
- `Device`: checagens de `jclass`/`jmethodID` nulos em `battery_level()`,
  `is_charging()` e `set_brightness()`, com liberação de refs locais
- `InputHelpers::reset()` agora zera também `mouse_x_`/`mouse_y_`

## [1.0.0] - 2026-11-22

Primeira versão estável do apkfast. Build Android completo sem Gradle, com
API nativa `nativeapp` e suporte a raylib/SDL2/SDL3.

### Adicionado

**CLI (`python3 apkfast.py`)**
- `build` — pipeline aapt2 → clang/NDK → aapt2 link → zipalign → apksigner
- `init` — assistente interativo (nome, pacote, template, ABIs, permissões,
  orientação, otimização, ofuscação, keystore) com modo `-y` para CI
- `doctor` — verificação de toolchain
- `devices` / `install` / `logs` / `info` — operação no dispositivo via adb
- `setup-sdk` — instala SDK/NDK pré-compilados para Termux
- `setup-libs` / `raylib-install` / `sdl-install` — bibliotecas sob demanda
- Multi-ABI (fat APK), `--release`, `--obfuscate`, `-q`/`--quiet`

**API `nativeapp` (header-only)**
- `App` — ciclo de vida (`Created → Ready ↔ Paused → Destroyed`), eventos de
  superfície, `on_update(dt)`, `set_scene()`, input polling
- `GLES2Scene` / `GLES3Scene` — `GLContext`, `Shader`, `Mesh`, `Texture`,
  matrizes e assets via `AAssetManager`
- `VulkanScene` — instance/device/surface/swapchain, depth buffer, render pass,
  `GraphicsPipeline` (SPIR-V), `GpuBuffer`, 2 frames in-flight, present modes
- `Permissions` / `JniPermissions` — runtime (API 21+), estados
  Granted/Denied/PermanentlyDenied, callbacks assíncronos
- `TextInput` — teclado virtual sem EditText nem Java (API 21+), unicode real
  de IME via JNI, backspace/enter
- `Audio` — AAudio (API 26+) com fallback OpenSLES, WAV de assets
- `Sensors` — acelerômetro/giroscópio via callbacks no looper do App
- `Device` — vibração, bateria, orientação, modelo, brilho
- `InputHelpers` — mouse, gamepad analógico, estado de teclas
- `math.hpp` — Mat4, projeções, look_at

**Exemplos (10)**
`native-hello`, `nativeapp-gl`, `nativeapp-vulkan`, `nativeapp-advanced`,
`permissions-test`, `raylib-hello`, `raylib-permissions`, `sdl2-hello`,
`sdl3-hello`, `sdl3-permissions`, `text-input-hello`, `device-demo`.

**Infraestrutura**
- MIT License, CONTRIBUTING.md, AUDIT.md (59 achados auditados e corrigidos)
- CI (GitHub Actions): sintaxe Python, help dos comandos, manifests XML
- `.gitignore` — nenhum binário versionado (repo com ~1.5 MB de .git)

### Corrigido

- Host-arch aware toolchain (binários x86_64 ignorados no Termux/arm64)
- Símbolos de linkagem: `-llog`, `-lEGL/-lGLESv2`, `-lvulkan` (sem LTO),
  `-lOpenSLES/-laaudio` com `--no-as-needed`
- `.so`/`.dex` `ZIP_STORED` + `zipalign -P 16` (Android 15+/16 KB pages)
- Tela preta ao voltar de background (recriação de surface/swapchain)
- JNI na thread errada, global refs, `string_view` não terminada
- WAV parser bounds, `SLDataSink`, volume
- Multi-toque, busy-loop, looper roubado por sensores
- Scaffolds SDL2/SDL3 (classes Java e constantes corretas)
- XML escape e validação de package no assistente

### Validado

- 10 exemplos compilam e rodam no dispositivo (SM-A266M, Android 16, API 36)
- 0 crash; geometria confirmada via `glReadPixels`; teclado via
  `dumpsys input_method`; assinatura v1/v2/v3 e `zipalign -c -P 16` OK
- Build com `--api 21` sem `-laaudio` OK
- CI verde
