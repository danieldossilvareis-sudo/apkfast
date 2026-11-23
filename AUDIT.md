# Auditoria de código (2026-11-22)

Auditoria completa de todo o código Python e C++ com dois agentes independentes.
59 achados. Correções aplicadas aos problemas com impacto real em estabilidade,
segurança e compatibilidade.

## Corrigido nesta rodada

### Build / Python
- `.so` e `.dex` agora são `ZIP_STORED` no APK (antes DEFLATED) — evita falha
  de `dlopen` em Android 15+/16 KB pages
- `zipalign -P 16` com fallback automático para versões antigas
- `--build-dir` não pode apagar o projeto nem um ancestral (validação antes de `_clean`)
- ABIs validadas antes de destruir o work dir
- `-laaudio` só é linkado quando `tc.api >= 26` (stub não existe em API 21-25)
- build-tools só é aceito se `aapt2`, `zipalign` E `apksigner` forem executáveis
- sort numérico de platforms (`android-36` > `android-9`) e NDK (`r30` > `r9`)
- `device.py`: checa `X_OK`, trata `OSError`, parse correto do `adb devices`,
  prefere USB real a emulador (código morto removido)
- `sdk_setup.py`: rollback seletivo (não apaga SDK pré-existente), aceita
  múltiplas licenças, normaliza versão `r30`/`30`
- `sdl.py`: `effective_api` consistente entre detect() e CMake
- `raylib.py`: `-DBUILD_EXAMPLES=OFF` (opção correta)
- `scaffold.py`: SDL2 usa `SDL_CreateRenderer(window, -1, 0)`;
  SDL3 usa `SDL_EVENT_QUIT` e `CreateWindow(0,0,0)`;
  classes Java do SDL copiadas automaticamente;
  `os.path.relpath` para a dica de build;
  XML com escape; `int()` validado no wizard; pacote validado nos dois caminhos
- mensagem de erro raylib usa `tc.abi` (não `options.abi == "all"`)
- imports não usados e código morto removidos

### C++ / API nativeapp
- **`sensor.hpp`: não rouba mais eventos do looper do App** — event queues
  registradas com callback no mesmo looper; `update()` agora só drena
- `device.hpp`: valida `jclass`/`jmethodID` antes de uso, libera refs locais,
  evita vazamento de ~2 refs/frame em `battery_level()`
- `permissions*.hpp`: `NewStringUTF` com `std::string` explícita (evita UB de
  `string_view` não terminada em NUL)
- `audio.hpp`: parser WAV valida tamanho de chunk (evita OOB/wrap), rejeita
  canais 0; `SLDataSink` corrigido com `SLDataLocator_OutputMix`; volume
  aplicado nos caminhos AAudio e OpenSLES; destrutor chama `destroy()`
- `app.hpp`: timeout `-1` quando pausado/sem janela (acaba com busy-loop 100%);
  multi-toque correto (`POINTER_DOWN/UP`, atualiza só o pointer afetado);
  `last_frame_` resetado no RESUME; `on_surface_destroyed()` recebe janela
  já nula; evita `on_surface_changed` duplicado; key events do sistema não
  são mais consumidos
- `gl.hpp`: `GLES3Scene` agora realmente pede contexto GLES3; `begin_frame()`
  restaura defaults de depth/blend; `.so`/textura/index limits endurecidos
- `assets.hpp`: checa `AAsset_getLength64() < 0` e leitura curta
- `input_helpers.hpp`: usa `AKEYCODE_*` em vez de números mágicos; `reset()`
  completo; `consume_scroll()` acumula delta
- `nativeapp.hpp`: exemplo do header corrigido (`on_surface_changed`)

### Exemplos
- `sdl2-hello`: removido `android_main()` morto (SDLActivity chama direto)
- `text-input-hello`: usa App+GLES (sem conflito de loops com raylib)
- scaffolds SDL2 e SDL3 validados do zero

## Validação
- 10 exemplos compilam
- APKs com `.so` STORED e `zipalign -c -P 16` OK
- Build com `--api 21` sem `-laaudio` OK
- Dispositivo: devicedemo, nativeapp-gl, nativeapp-vulkan, permstest — vivos, 0 crash
- Sensor via callback: `accel≈9.74` sem roubar eventos do App
- TextInput: teclado abre, digitação chega
- CI verde

## Rodada final (conclusão das correções)

Últimos itens fechados sem alterar comportamento já validado:
- `Permissions::shutdown()` / `JniPermissions::shutdown()` chamados pelo
  `App::finish()` — libera global refs mesmo se a subclasse sobrescrever
  `on_destroy()` sem chamar a base
- scaffold: `xml.sax.saxutils.quoteattr()` em nome/package/permissões do
  manifest (nome `Jogo & Teste` gera `Jogo &amp; Teste`, XML válido)
- scaffold: `_ask_int()` com retry e validação de package também no modo `-y`
  (`3d-game` → `com.example.app3dgame`)
- `GLES2Scene::init()` agora deriva viewport da janela automaticamente
- `MeshData::add_triangle()` rejeita malhas acima de 65535 vértices
- textura que falha em `load()` não fica cacheada como textura 0
- `draw_mesh()` desativa os 4 attribute arrays após o draw
- `mat4_look_at()` usa up alternativo quando paralelo à direção de visão
- `Device::orientation()` usa `Display.getRotation()` (multi-window safe)
- `Device::set_brightness()` clampa para `[-1, 1]`

Validação extra: app com nome `Jogo & Teste` e diretório `3d-game` gera
manifesto XML válido e compila; 4 apps no dispositivo vivos, 0 crash.

## Regressão de tela preta (GL/VK) — diagnosticada e corrigida

Após a auditoria, GL e Vulkan ficaram com tela preta. Investigação com
`glReadPixels` provou que o framebuffer **continha** geometria (11250–17648
pixels não-background) — o `screencap` do adb **não captura o conteúdo EGL
neste dispositivo Samsung**, o que gerou falsa impressão de tela preta.

Causa real da regressão visual relatada: `GLES2Scene::render()` usava a
projection ortho configurada por `resize()` (coordenadas de tela) com a mesh
legada `default_mesh()` (coordenadas NDC -0.5..0.5) — a geometria ficava
fora da área visível. Corrigido salvando/restaurando a projection em
`render()` e usando identidade para a mesh legada.

Também confirmado que `App::run()` com `timeout=0` e processamento ilimitado
de eventos (já no HEAD) evita o travamento do `INIT_WINDOW` que causava
`on_draw` nunca ser chamado.

Validação final: 10 exemplos compilam; GL e VK no dispositivo com PID ativo,
0 crash; `glReadPixels` confirma geometria no framebuffer.

TextInput também confirmado via `glReadPixels`: `nonbg=40000` pixels (exatamente
o quadrado 200×200 do exemplo), `mInputShown=true` no `dumpsys input_method`,
texto chegando via callback. O "não desenha/não abre teclado" relatado era
limitação do `screencap` do adb neste dispositivo Samsung, que captura apenas
o clear color das surfaces EGL — não o conteúdo renderizado.

## Não corrigido (aceito / fora de escopo)
- `stb_image.h` (terceirizado) continua com `STB_IMAGE_IMPLEMENTATION` no header
  público — gera código por TU, mas funciona e é opt-in implícito
- `GLES3Scene::draw_instanced()` ainda usa loop de uniforms em vez de
  `glDrawArraysInstanced` (compatibilidade GLES2)
- AAudio é compile-time (`__ANDROID_API__ >= 26`); com minSdk 21 em device 26+
  cai para OpenSLES — correção exigiria `dlopen/dlsym`
- Race conditions com builds paralelos no mesmo diretório (baixa probabilidade)
- Algumas checagens JNI adicionais em casos extremos de exceção pendente

## v2.4.0 — EQUIPE A

Escopo: itens que ficaram de fora da v2.3.0 (transporte de cache, SVG
declarativo animado e redução do trabalho restante no build). Nenhuma versão
foi alterada e nenhuma API existente foi removida.

### Feito

- `apkfast/cache_transport.py`: `register()`, `push()`, `pull()` e
  `TransportResult`. Reusa `cache.export_bundle()` / `cache.import_bundle()`
  sem criar segundo formato; suporta path local/`file://` com cópia atômica,
  HTTP(S) com `curl -fL --retry 3` (PUT/GET, `.part` + `rename`, redirect
  restrito a http/https, `--header` repitível) e `s3://` via `aws s3 cp`.
  Respeita `APKFAST_CACHE_HMAC` e `--require-cache-signature`; oferece
  `--expect-sha256` opcional (SHA256 do arquivo, nunca ETag S3). Locks
  `(projectId, ABI)` envolvem export/import.
- `include/nativeapp/animated_svg.hpp` + implementação em `image_impl.hpp`:
  `AnimatedSvg` com pimpl (nenhum tipo LunaSVG vaza para a API pública),
  `open`/`open_asset`, `update(dt_ms)`, `reset`, `width/height`,
  `duration_ms`, `loop_count`, `animation_count`, `set_size` e `current()`.
  Driver SMIL lê `<animate>`, `<animateTransform>` e `<set>` do XML cru
  (o parser do LunaSVG descarta elementos desconhecidos) e reaplica atributos
  no alvo resolvido por `href="#id"`, `id` ancestral ou tag ancestral+índice.
  Documentos sem SMIL seguem exatamente o caminho `decode_svg_auto()`.
- Documentação nova: `docs/cache-transport.md`, `docs/animated-svg.md`;
  duas linhas adicionadas às tabelas de `docs/README.md`.

### Riscos e limitações honestas

- A integração em `cli.py`, `nativeapp.hpp`, `builder.py`, `CHANGELOG.md` e
  `README.md` é de responsabilidade da sessão coordenadora: até lá os
  subcomandos `cache-push`/`cache-pull` não aparecem na CLI e `AnimatedSvg`
  precisa ser incluído explicitamente (o build já compila LunaSVG e o TU
  injetado, então nenhuma flag nova é necessária).
- SMIL é um subconjunto: sem `<script>`, CSS animations, `calcMode` não linear,
  `keyTimes`/`keySplines`, `animateMotion`, `animateColor`, eventos/`begin`
  com syncbase, `end`/`min`/`max` ou `href` externo. Atributos não numéricos
  só funcionam via `<set>`. Ver `docs/animated-svg.md` para a lista completa.
- Sem LunaSVG instalado, um SVG com SMIL **falha** em `open()` (aviso no
  logcat) em vez de cair silenciosamente para NanoSVG estático. Com LunaSVG,
  marcadores que não pertencem ao subconjunto são ignorados; se nenhum for
  utilizável, `open()` faz fallback para o decoding estático e avisa.
- Cada mudança de atributo animado rasteriza o documento inteiro; animações
  com muitos elementos podem ser caras em dispositivos little-endian de baixo
  poder. Não há cache de frame SVG (diferente de `AnimatedImage`).
- HTTP PUT depende de endpoint que aceite `PUT`; atomicidade remota é
  responsabilidade do servidor. Somente destinos locais/`file://` usam
  `os.replace` de fato.
- `aws s3 cp` exige AWS CLI no PATH; ETags nunca são usados como integridade
  (correção/autenticidade vêm do SHA256 opcional e do HMAC do manifesto).
- Item (3) do escopo (cache de recursos/APK assinado) **não foi implementado**:
  mexeria em `builder.py`/`apkfast.py`, que são de propriedade da sessão
  coordenadora. Nenhum atalho de build foi introduzido nesta rodada.
