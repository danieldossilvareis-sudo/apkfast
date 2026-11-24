# Changelog

## [2.7.3] - 2026-11-24 — Documentação auditada

Nenhuma mudança de runtime, API ou build. A base permanece estável e congelada.

- README: versões corretas de SDL2 (2.32.10), SDL3 (3.4.20) e raylib (5.5);
  `vendor/` não versiona binários — use `setup-libs`
- README: pipeline interno atualizado (`zipalign -P 16`, v1/v2/v3/v4, AAB)
- `docs/compatibility.md`: contrato atualizado para API 2.7.3
- `docs/migration.md`: seções 2.4→2.5, 2.5→2.6 e 2.6→2.7
- `docs/publishing.md`: estado real do PyPI e SemVer examples
- `docs/cache-transport.md`: exemplos usando a CLI já integrada

## [2.7.2] - 2026-11-24 — Estável

- metadado do PyPI promovido de **Beta** para **Production/Stable**
- sem mudanças de runtime, API ou build

Status oficial: **estável**.

## [2.7.1] - 2026-11-24 — Correção da documentação PyPI

- README publicado no PyPI não dizia mais que o pacote “ainda não foi publicado”
- seção de instalação agora recomenda `pip install apkfast`
- sem mudanças de runtime, API ou build

## [2.7.0] - 2026-11-23 — Cache completo

Últimos itens internos que não dependem de infraestrutura externa.

### Adicionado — Equipe A

- cache de **D8/R8** (`classes.dex`) com chave por fontes Java, flags, min SDK
  e versão do d8/r8
- cache de **AAB assinado** com chave por recursos, dex, natives por ABI,
  versão do bundletool, keystore e flags de assinatura
- **chaves por ABI**: `--abi arm64-v8a` reaproveita entradas geradas por
  `--abi all`; natives entram como mapa `<abi> -> hash`
- `export_bundle`/`import_bundle` transportam também `resource`, `dex`,
  `apkLink`, `apk` e `aab` (formato aditivo)
- eventos `stage='dex'` e `stage='aab'` no build report

### Adicionado — Equipe B

- `versionCode` timestamp seguro: segundos desde **2020-01-01 UTC**,
  monotônico e válido até **2086-07-18** dentro do limite do Play
- documentação atualizada em `docs/signing.md`

### Validado

- 93 testes unitários OK
- APK: 1º build com misses, 2º com 5 hits (sem javac/D8)
- AAB: 2º build com 6 hits e nenhuma invocação de bundletool
- keystore diferente invalida `apk` e `aab` com motivo explícito
- `--abi all` reaproveita dex da ABI isolada

### Limitações restantes

- fingerprint da keystore é SHA256 do arquivo, não do certificado
- hit de AAB pula a verificação local (`bundletool validate`)
- apps que já publicaram versionCode alto devem usar `explicit`

## [2.6.0] - 2026-11-23 — Robustez final

Quatro itens de endurecimento do backend.

### Adicionado — Equipe A

- **Pinning de fontes**: raylib 5.5, SDL2 2.32.10 e SDL3 3.4.20 com SHA256
  verificado; overrides controlados por `APKFAST_*_VERSION` / `_SHA256`
- `apkfast/signing.py`:
  - assinatura v1/v2/v3/v4 com degradação segura e flags CLI
  - keystore/senhas/alias por variáveis de ambiente
  - `versionCode` automático (`manifest+1`, `explicit`, `timestamp`)
  - fingerprint da keystore para chave de cache
- `.idsig` (v4) copiado junto do APK final
- `docs/signing.md`

### Adicionado — Equipe B

- **Cache de recursos e APK assinado**: `aapt2 compile`, `aapt2 link` e
  `zipalign + apksigner` reaproveitados quando chave e keystore batem
- trocar keystore invalida só o APK assinado; unsigned continua em cache
- **Diagnóstico**: `build --report` agora traz `cache.events` com motivo de
  hit/miss e `timings_ms` por etapa
- `docs/build-cache.md`

### Validado

- 82 testes unitários OK
- 14 exemplos compilam
- build real: primeiro build com misses, segundo com hits em `native`/`link`/
  `apk`; `sign_ms` cai de ~2881 ms para ~26 ms
- AAB continua gerado e validado com bundletool

### Limitações restantes

- D8/R8 e AAB não são cacheados
- chaves de cache por combinação exata de ABIs
- `timestamp` usa segundos desde 2020; seguro até 2086 (limite 2.1e9 do Play)
- GitHub Actions ainda não publica no PyPI nem roda dispositivo real sem
  credenciais/runner configurados

## [2.5.0] - 2026-11-23 — Backend completo

Fecha os três bloqueadores restantes de distribuição/qualidade.

### Adicionado — Equipe A (AAB)

- **AAB (Android App Bundle)** via bundletool 1.18.3 pinned (SHA256 verificado)
- `build --aab`, `--aab-only`, `--aab-output`, `--bundletool-version`
- montagem do `base.zip`, link `aapt2 --proto-format`, assinatura AAB com
  `jarsigner` e verificação com `bundletool validate`/`build-apks`
- correção de bug no aapt2 2.20: `.flat` vinha dentro de um ZIP e era extraído
  antes do link (afetava qualquer projeto com `res/`)
- `docs/aab.md`

### Adicionado — Equipe B (distribuição/qualidade)

- comandos `dist` e `publish-pypi` com dry-run por padrão, `--yes` para upload,
  token apenas via ambiente e redação de segredos
- `.github/workflows/publish.yml`: release → build/twine check → PyPI/TestPyPI
  via OIDC ou `PYPI_API_TOKEN`
- `.github/workflows/device-real.yml`: modos self-hosted (KVM/aparelho) e
  Firebase Test Lab, com skip explícito quando não configurado
- `scripts/device-check.sh` para validação adb local
- `docs/publishing.md`, `docs/device-ci.md`

### Validado

- 53 testes unitários OK
- AAB real: `native-hello`, `image-demo` e fat APK (`--abi all`) validados com
  `bundletool validate` e `build-apks --mode=universal`
- `jarsigner -verify` OK
- script `device-check.sh` com adb falso: caminho feliz e detecção de `FATAL`

### Ainda externo

- Upload real ao PyPI depende de credenciais/OIDC configuradas; automação pronta
- CI de dispositivo real depende de runner self-hosted ou projeto Firebase

## [2.4.1] - 2026-11-23 — Patch de estabilidade

- CI convertida de emulator para **build matrix** confiável: API 21/28/35 ×
  x86_64 e API 35 × arm64-v8a/armeabi-v7a (todas verdes)
- `setup-libs`: raylib compilada em API 21 para funcionar com NDK 27.x
- `webp`: SIMD habilitado em x86/x86_64 (corrige build da libwebp no CI)
- `text_input.hpp`: declaração compatível de `AInputEvent_toJava` com NDKs
  antigos que suportam API 35
- `vk.hpp`: `VK_NULL_HANDLE` em vez de `nullptr` — corrige 32-bit ABIs com
  handles Vulkan de 64 bits
- supersede a v2.4.0: use 2.4.1 como versão estável da linha 2.4

## [2.4.0] - 2026-11-23 — Base pronta e estável

Duas frentes paralelas: **limites da 2.3.0** e **consolidação da base**.

### Adicionado — limites removidos

- `cache-push` / `cache-pull`: transporte do bundle `tar.gz` por path local,
  `file://`, HTTP(S) (`curl -fL --retry 3`) e `s3://` (`aws s3 cp`), com HMAC e
  `--expect-sha256`. Nenhum segundo formato de cache foi criado.
- `nativeapp::AnimatedSvg`: subconjunto SMIL (`animate`, `animateTransform`,
  `set`) sobre LunaSVG, com `update(dt_ms)`, `reset()`, `current()` e
  `set_size()`. SVGs sem SMIL continuam no caminho estático.
- `docs/cache-transport.md` e `docs/animated-svg.md`.

### Adicionado — consolidação

- **API versionada**: `include/nativeapp/version.hpp` com
  `APKFAST_API_VERSION_*` e `APKFAST_DEPRECATED`; política em
  `docs/compatibility.md` e guia em `docs/migration.md`
- **Lockfile reprodutível**: `apkfast.lock.json` com NDK/build-tools/platform,
  versões de libwebp/LunaSVG e checksums; `--generate-lock`, `--ignore-lock`
- **Config validada**: `apkfast/config_schema.py`, `apkfast config show`
- **Observabilidade**: `build --report`, `doctor --json`
- **Instalação**: `pyproject.toml`, entry point `apkfast`, `pip install -e .`,
  wheel/sdist com `include/`, `src/`, `patches/` e `docs/`
- **Completions**: bash/zsh/fish via `--completions` ou `completions`
- **Matriz de build no CI**: `.github/workflows/device.yml` com API 21/28/35
  × x86_64 e API 35 × arm64-v8a/armeabi-v7a; validação em dispositivo fica
  local (runners GitHub-hosted não bootam emuladores ARM de forma confiável)
- Governança: `SECURITY.md`, `CODE_OF_CONDUCT.md`, `THIRD_PARTY_NOTICES.md`,
  `RELEASE.md`

### Validado

- 29 testes unitários OK
- 14 exemplos compilam
- `pip install -e .`, wheel e sdist validados
- CLI: `config show`, `doctor --json`, completions, `cache-push/pull --help`

### Limitações honestas

- SMIL é subconjunto: sem `<script>`, CSS animations, `calcMode` não linear,
  `keyTimes`, `animateMotion` ou `href` externo
- HTTP PUT depende do servidor; atomicidade remota é responsabilidade dele
- `vendor/` não entra na wheel — libs avançadas são baixadas por `setup-libs`
- `build --report` fica fora do APK e não é assinado

## [2.3.0] - 2026-11-23 — Limitações removidas

### Adicionado
- **WebP animado completo** via libwebp 1.6.0 (`webp-install`, `setup-libs`):
  `AnimatedImage` com todos os frames, duração, loop, blend/dispose e
  `update(dt_ms)`; `draw_with_texture()` para texturas externas
- **SVG completo** via LunaSVG 3.5.0 + patch de filtros (`lunasvg-install`):
  CSS, texto com TTF de assets, clipPath, mask, pattern e filtros SVG 1.1;
  NanoSVG permanece como fallback para ícones simples
- `svg_register_font_asset()`, `decode_svg_full()`, `decode_svg_auto()`
- **Cache remoto real**: schema 2 com chaves portáveis (paths lógicos,
  `-ffile-prefix-map`), raiz configurável `--cache-dir`/`cacheDir`,
  modos `trusted`/`verify`, locks por `(projectId, ABI)` e comandos
  `cache-export`/`cache-import` com bundle `tar.gz`, HMAC e validação de ELF
- `docs/cache-remote.md`; `docs/incremental.md` reescrito

### Corrigido
- `MeshData::quad()` usava V invertido: texturas apareciam de cabeça para baixo.
  Agora stb/libwebp/LunaSVG renderizam na orientação correta.

### Invalidado
- Cache da v2.2.0 (schema 1) é descartado uma única vez pelo schema 2.

### Validado
- dispositivo: `webp=OK 64x64 svg=OK 256x256 font=OK anim=OK frames=3 loop=0`
- frames do WebP animado avançando (1/3 → 2/3 → 3/3), `0 FATAL`
- SVG com `<style>`, `!important`, `<text>`, clipPath, mask, pattern e
  `feDropShadow` renderizado corretamente
- cache exportado num root e importado noutro reaproveita TUs e link sem chamar
  clang/llvm-strip

## [2.2.0] - 2026-11-23 — Build incremental + WebP/SVG

### Adicionado
- **Build incremental** com cache por projeto/ABI fora do `work`:
  hash da fonte, flags, toolchain, headers do nativeapp, depfiles (`-MD -MF`)
  e link cacheado. Flags `--no-incremental` e `--clean-cache`, chaves
  `incremental`/`cleanCache` em `apkfast.json`. Guia `docs/incremental.md`
- **WebP** via `BitmapFactory` + `AndroidBitmap_*` (JNI puro, sem Java
  customizado), com alpha premultiplicado corrigido e stride respeitado
- **SVG** via NanoSVG vendorizado (licença zlib), rasterizado para RGBA com
  tamanho natural ou `width/height` explícitos
- `Texture::load()` detecta formato por magic bytes; novos métodos
  `load_webp_asset()`, `load_svg_asset()` e `load_rgba()`
- `image_codecs.hpp`, `image_impl.hpp`, `jni_bitmap.hpp`
- builder linka `-ljnigraphics` automaticamente
- exemplo `examples/image-demo` com WebP e SVG

### Validado
- 14 exemplos compilam (incluindo raylib/SDL, que usam o TU sem `stbi_*`)
- dispositivo: `I image-demo: webp=OK 64x64 svg=OK 256x256`, `0 FATAL`
- cache quente: nenhuma chamada a clang/llvm-strip; source alterada
  recompila só aquela TU

### Notas
- WebP animado: apenas primeiro frame
- SVG cobre paths/formas/gradientes simples (sem CSS/texto/filtros)

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
