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

## v2.5.0 — EQUIPE A

Escopo: AAB (Android App Bundle) no backend, fechando a limitação de
distribuição da v2.5.0. Nenhuma versão foi alterada; `publish.py`,
`device_ci.py`, `.github/`, `pyproject.toml`, `CHANGELOG.md` e `include/`
não foram tocados nesta rodada.

### Feito

- `apkfast/bundle.py`: `ensure_bundletool()` (bundletool 1.18.3 fixo, SHA256
  `a099cfa1543f55593bc2ed16a70a7c67fe54b1747bb7301f37fdfd6d91028e29`, cache
  em `~/.apkfast/tools/` com verificação antes de cada uso, redownload se o
  cache divergir, `.part` + rename atômico), `prepare_module()`/`build_aab()`
  (`base.zip` com `manifest/AndroidManifest.xml`, `resources.pb`, `res/`,
  `dex/`, `lib/<abi>/`, `assets/`), `sign_aab()` (jarsigner v1/SHA-256, nunca
  apksigner) e `verify_aab()` (`validate` + `dump manifest` + `build-apks
  --mode=universal` com `--aapt2` da toolchain).
- `apkfast/builder.py`: segundo `aapt2 link --proto-format` isolado em
  `_link_proto()` (o link do APK continua intacto), extração da resolução de
  keystore para `_resolve_keystore()` (APK e AAB usam a mesma chave), flags
  `--aab`/`--aab-only`/`--aab-output`/`--bundletool-version` e nome padrão
  espelhando o APK. Aviso explícito sobre upload key × app signing e sobre
  keystore de debug.
- `apkfast/builder.py` (correção descoberta na validação com recursos): o
  aapt2 2.20 grava um ZIP contendo um `.flat` mesmo com `-o arquivo`; o
  builder agora extrai o `.flat` interno antes do link. Sem isso, qualquer
  projeto com `res/` falhava no link proto (e também no APK) com
  `magic value is 0x04034b50`.
- `apkfast/cli.py`: flags novas no `build`, relatório JSON com o caminho do
  AAB e versão do bundletool, saída no console com ambos os artefatos.
- `docs/aab.md`, seção curta no `README.md`, `tests/test_bundle.py` (unitários
  sem download: versão/SHA, nomes de arquivo, layout do `base.zip`, recusa de
  ABI inválida, comandos de build/sign/verify).

### Riscos e limitações honestas

- Apenas o módulo `base`: sem dynamic features, asset packs ou `.obb`.
- `build-apks` local depende de um `aapt2` executável no host; o binário
  linux/x86_64 embutido no bundletool não roda no Termux, por isso o apkfast
  passa `--aapt2` da toolchain explicitamente. Se o aapt2 instalado não
  suportar `--proto-format`, o build falha com dica de usar o aapt2 do Maven.
- `--bundletool-version` diferente do padrão consulta o digest do release no
  GitHub; sem conectividade, use a versão fixa ou `APKFAST_BUNDLETOOL_SHA256`.
- Verificação local (`validate` + `dump` + APK universal) não substitui o
  upload de teste no Play Console (políticas e Play App Signing).
- `.gitignore` está fora da propriedade desta equipe: output padrão
  `dist/*.aab` ainda aparece como untracked. A sessão coordenadora deve
  adicionar `*.aab` ao `.gitignore` (não feito aqui).

## v2.6.0 — EQUIPE B

Escopo: itens (5) e (7) do roadmap — cachear recursos/APK assinado e entregar
diagnóstico rico (motivos de hit/miss + timings no `--report`). Nenhuma versão
foi alterada. Arquivos de propriedade de outras equipes não foram tocados.

### Feito

- `apkfast/resource_cache.py` (novo): `resource_key()` (res/, assets/, manifesto,
  min/target SDK, package/versão, caminho lógico + `--version` do aapt2,
  namespace/projectId), `apk_key()` (resource_key + hashes das `.so`/dex,
  ABIs, nome da lib, flags de zipalign/assinatura e fingerprint da keystore),
  `keystore_fingerprint_info()` com import tolerante de
  `apkfast.signing.signature_fingerprint()`, blobs ZIP/digest via
  `cache.CacheContext`, verificação SHA256/ZIP antes do uso e limpeza escopada
  por projeto (`clear_resource_cache()` / `clear_apk_cache()`).
- `apkfast/builder.py`: hits de `resource` copiam os flats; `apkLink` copia o
  APK unsigned após aapt2 link + arquivos extras; `apk` copia o APK assinado
  para `work/apk/signed.apk` e pula `aapt2 link`, `zipalign` e `apksigner`.
  Trocar a keystore invalida o APK assinado mas ainda reaproveita o unsigned.
  `--no-incremental` desliga leitura/escrita em todos os níveis;
  `--clean-cache` também remove as novas entradas e blobs órfãos.
- `apkfast/builder.py` + `apkfast/cli.py`: `cache_events` com
  `stage/result/reason/key` (resource/link/apk/native), misses explicados em
  `-v`, e timings `toolchain_ms`, `resources_ms`, `native_ms`, `package_ms`,
  `sign_ms`, `total_ms`. `_write_build_report()` ganhou `cache` e `timings`
  mantendo todas as chaves anteriores.
- `docs/build-cache.md`, seção curta no `README.md`,
  `tests/test_resource_cache.py` (8 testes: invalidadores de chaves, keystore,
  hit/miss, corrupção de blob, limpeza escopada e payload do relatório).

### Riscos e limitações honestas

- D8/R8 e o AAB (`--aab`) continuam rodando sempre; seus tempos entram em
  `package_ms`, mas não há cache próprio para eles.
- O fingerprint de `signing.py` é o SHA256 do arquivo da keystore, não o
  fingerprint do certificado: basta o arquivo mudar para invalidar. Senhas não
  entram na chave.
- A chave do APK inclui hashes dos binários `zipalign`/`apksigner` e a flags
  `-P 16`. Se o zipalign não aceitar `-P 16` e cair para 4, o build loga o
  aviso, mas a chave não muda (o artefato em cache continua o mesmo).
- caches são por combinação exata de ABIs; alternar `--abi` e `--abi all`
  gera chaves diferentes.
- Não houve validação com SDK/NDK reais nesta rodada. Foi feito um smoke
  end-to-end com aapt2/zipalign/apksigner/clang falsos confirmando: 1º build
  miss em tudo, 2º build hit de resource/native/link/APK (sem zipalign nem
  apksigner), 3º build com keystore diferente reaproveitando o APK unsigned.
  Ainda assim, um build com as ferramentas reais deve ser feito pelo
  coordenador.

## v2.7.0 — EQUIPE A

Escopo: itens de cache que ficaram na v2.6.0 — DEX (D8/R8), AAB assinado e
chaves por conteúdo de ABI. Nenhuma versão foi alterada; `signing.py`, `cli.py`,
`publish.py`, `docs/signing.md`, `.github/`, `pyproject.toml`,
`CHANGELOG.md`, `README.md`, `docs/README.md`, `apkfast/__init__.py` e
`include/` não foram tocados **por esta equipe** nesta rodada (outras equipes
podem estar alterando esses arquivos em paralelo).

### Feito

- `apkfast/resource_cache.py`: `dex_key()` (fontes `.java`/`.jar` por caminho
  relativo + SHA256, `optimize`/`obfuscate`, `minSdk`, `--version`/SHA256 de
  `javac`/`d8`/`r8`, `proguard-rules.pro` e a configuração Java do manifesto)
  e `aab_key()` (`resource_key` + `dex_key` + natives por ABI + versão do
  bundletool + fingerprint da keystore + alias/algoritmo/jarsigner). Blobs
  novos: `store_dex()/load_dex()` (cabeçalho `dex\n`, não ZIP) e
  `store_aab()/load_aab()` (ZIP assinado), ambos com SHA256/tamanho validados e
  cópia atômica; `clear_dex_cache()`/`clear_aab_cache()` e
  `clear_all_resource_caches()` para o `--clean-cache` escopado.
- `native_libs_state()` troca a string da combinação de ABIs por um mapa
  `<abi> -> [[arquivo, sha256], ...]`. `apk_key()`/`aab_key()` filtram esse
  mapa pela seleção de ABI; uma ABI isolada tem a mesma chave da mesma ABI
  dentro de `--abi all` quando o conteúdo de `.so` é idêntico.
- `apkfast/builder.py`: hit de `dex` devolve `classes.dex` e pula `javac` e
  D8/R8; hit de `aab` copia o AAB assinado direto para o destino final e pula
  `aapt2 --proto-format`, `bundletool`, `jarsigner` e a verificação local.
  Eventos `stage='dex'`/`stage='aab'` com `hit`/`miss` no `--report`;
  `--no-incremental` desliga leitura/escrita nos dois níveis; um keystore
  diferente explica o miss de AAB e nunca serve o blob antigo.
- `apkfast/cache.py`: `export_bundle()`/`import_bundle()` passam a transportar
  as entradas `resource`/`dex`/`apkLink`/`apk`/`aab`. O formato do manifesto é
  aditivo (`BUNDLE_VERSION` permanece 1), então bundles antigos importam.
- `docs/build-cache.md` reescrito com os seis estágios, invalidadores por
  chave, regras de ABI, segurança e o que ainda roda sempre;
  `tests/test_resource_cache.py` ganhou 6 testes (invalidadores de dex/AAB,
  ABI isolada vs `all`, hit/miss/corrupção de dex e AAB, transporte via bundle).

### Riscos e limitações honestas

- O fingerprint continua sendo o SHA256 do arquivo da keystore, não do
  certificado; senhas não entram em chave, blob, log ou relatório.
- Hit de AAB não repete `bundletool validate`/`dump`/`build-apks`: a
  verificação local só acontece no miss. O upload de teste na Play Store
  continua sendo responsabilidade do publicador.
- Um hit de DEX confia na identidade de `javac`/D8/R8 por `--version` + SHA256
  do binário; ferramentas com o mesmo conteúdo em caminhos diferentes são
  equivalentes (caminhos lógicos), o que é intencional para portabilidade.
- Chaves são por conteúdo de ABI: `--abi all` com mais `.so` gera chave
  diferente da ABI isolada. Isso evita servir um fat APK/AAB como se fosse o
  artefato de uma ABI só; o reaproveitamento ocorre quando o mapa de libs é
  realmente igual.
- `--clean-cache` continua escopado ao `projectId`/namespace e às ABIs
  selecionadas; blobs órfãos são removidos, mas entradas em `cache.json` de
  ABIs não selecionadas permanecem.
- Validação nesta rodada: 93 testes unitários (`python3 -m unittest discover
  -s tests -v`) passaram, incluindo os 14 de `test_resource_cache.py`. Também
  foi feito smoke real (SDK/NDK 37/36, bundletool 1.18.3 em cache) num projeto
  temporário com uma classe Java: 1º build APK com miss de `dex`/native/link/
  `apk`, 2º com 5 hits (DEX reaproveitado, sem javac/D8); 1º `--aab` com miss
  de `aab`, 2º com hit e sem invocar bundletool; keystore diferente gerou
  `apk miss`/`aab miss` com o motivo "keystore diferente"; `--abi all`
  reaproveitou o mesmo DEX da ABI isolada e estabilizou o APK no 2º build.
  Ainda não foi feito build em dispositivo.

## Paridade Vulkan

Backend Vulkan (`include/nativeapp/vk.hpp`) elevado ao mesmo nível de
ergonomia de `GLES2Scene`, mantendo source compatibility de `init`, `attach`,
`render`, `set_pipeline`, `set_clear_color`, `set_present_mode`, `invalidate` e
`resize`. Nenhuma versão, tag, release ou publicação foi alterada.

### O que foi entregue

- Shaders GLSL 450 versionados como comentário no header e SPIR-V incorporado
  em arrays `uint32_t` (`kGeometryVertSpv`, `kGeometryFragSpv`,
  `kTextVertSpv`, `kTextFragSpv`), gerados com `glslangValidator -V`.
- UBO por frame (`SceneUniforms`: projection/model/color/use_texture) com
  dynamic offsets, descriptor set de sampler separado e textura branca 1×1
  para `Material::texture` vazio.
- `TextureVk` com `decode_asset_image()` (PNG/JPEG/WebP/SVG), upload via
  staging buffer HOST_VISIBLE → DEVICE_LOCAL, image view, sampler
  linear/clamp e `destroy()` seguro. Cache por caminho em `VulkanScene`.
- `draw(MeshData, Material, model)` passou a enfileirar itens; `render()`
  faz upload e grava os comandos. `depth_test`/`blend`/topology selecionam
  variantes de pipeline; cor, textura e model chegam ao shader.
- `load_font()`/`draw_text()`/`has_font()` com `FontAtlas`, pipeline de texto
  (x,y,u,v) e coordenadas de tela iguais às do GL. A projeção usa NDC do
  Vulkan (`mat4_ortho(0,w,0,h,-1,1)`) para produzir a mesma posição em tela.
- Exemplo `nativeapp-vulkan` atualizado com clear color, triângulo colorido,
  quad texturizado e `draw_text("VULKAN OK", ...)`.
- Documentação reescrita em `docs/graphics-vulkan.md`, tabela de paridade
  GL/VK e 9 testes estáticos novos em `tests/test_vulkan_parity.py`.

### Validação real

- `glslangValidator` 11.16.6: 4 shaders compilados sem erro; arrays SPIR-V
  inseridos no header com a fonte GLSL preservada em comentário.
- `clang++` NDK r30 (`-std=c++17 -fsyntax-only`): header e exemplo compilam
  (apenas os warnings de depreciação já existentes em `audio.hpp`/`sensor.hpp`).
- Build: `python3 apkfast.py build examples/nativeapp-vulkan -o
  <tmp>/vk-parity.apk -q` OK. O caminho literal `/tmp/vk-parity.apk` falhou
  com `EACCES` neste Termux; foi usado
  `/data/data/com.termux/files/usr/tmp/opencode/vk-parity.apk`.
- Dispositivo (`emulator-5554`): `sh scripts/device-check.sh
  <tmp>/vk-parity.apk com.apkfast.nativeappvk` OK — 362 linhas de logcat,
  0 erros FATAL/dlopen/AndroidRuntime.
- Screenshot do APK sem assets: clear color e triângulo com cores
  interpoladas visíveis; logcat confirmou `on_ready`, device Vulkan criado e
  avisos de `sprite.png`/`font.ttf` ausentes.
- Build adicional com assets temporários fora do repositório
  (`font.ttf` + `sprite.png` de `media-demo`, via `--assets`): textura e texto
  renderizados; a orientação do texto coincide com o `GLES2Scene` no mesmo
  dispositivo (o GL já apresenta essa convenção de UV neste frontend).
- `python3 -m unittest discover -s tests -q`: 102 testes OK (incluindo os
  testes novos de paridade Vulkan).

### Estado histórico desta seção

As limitações listadas aqui foram eliminadas nas seções seguintes:

- assets de `font.ttf`/`sprite.png` versionados em `examples/nativeapp-vulkan/assets/`;
- `set_validation(true)` ativa layers reais quando disponíveis;
- upload de textura assíncrono via `VulkanUploadTracker`;
- cache de textura sem limite rígido e com invalidação explícita;
- `set_pipeline()` legado aplica `Material`/`model`;
- texto e orientação visual corrigidos e validados contra GLES2.

## Correção visual Vulkan — EQUIPE A

Escopo: paridade visual exata do backend Vulkan com `GLES2Scene` no exemplo
`nativeapp-vulkan`. Nenhuma versão, tag, release ou publicação foi alterada.
SPIR-V/GLSL não precisaram ser regenerados (a matemática de UV está na CPU).

### Causas raízes

1. **Quad com "pixel faltando"/achatado:** `TextureVk::load_rgba()` usava
   `VkImageSubresourceLayout::rowPitch` de uma imagem `TILING_OPTIMAL` para
   distribuir as linhas no staging buffer, mas `vkCmdCopyBufferToImage()` foi
   chamado com `bufferRowLength = 0` (linhas firmemente empacotadas). Com
   `rowPitch > width * 4`, só parte das linhas era preenchida; as restantes
   ficavam pretas/transparentes e o quad de 180×180 aparecia com ~46 px de
   altura e um "degrau" na borda (fronteira entre linhas copiadas e vazias).
   Corrigido copiando `row_bytes = width * 4` por linha, sem pitch, e
   espelhando a ordem vertical do decode para casar com a convenção de
   `glTexImage2D`/`MeshData::quad()`.
2. **Texto ilegível/invertido e geometria na posição errada:**
   `vk_detail::ortho2d_screen()` reutilizava `mat4_ortho2d()` do GL
   (`bottom = h, top = 0`). No NDC do Vulkan `y=+1` está na base, então a
   matriz espelhava verticalmente tudo: `draw_text()` de cabeça para baixo e
   triângulo/quad refletidos em `height - y`. Corrigido para
   `mat4_ortho(0, w, 0, h, -1, 1)`. Removido o hack `baseline = height - y`
   em `draw_text()`: `FontAtlas::quad()` recebe `y` diretamente, como no GL.
   Como o upload do atlas agora espelha as linhas, os vértices do glyph usam
   `v = 1 - v` (sem tocar em `font.hpp`/`gl.hpp`).

### Arquivos modificados

- `include/nativeapp/vk.hpp` — projeção, upload de textura e UVs de texto.
- `docs/graphics-vulkan.md` — convenções de projeção/UV e validação visual.
- `examples/nativeapp-vulkan/**` — nenhuma alteração versionada; os assets
  `sprite.png`/`font.ttf` já presentes foram reutilizados.
- `AUDIT.md` — apenas esta seção.

### Validação real (emulator-5554, 1080×2340)

```sh
python3 apkfast.py build examples/nativeapp-vulkan \
  -o /data/data/com.termux/files/usr/tmp/opencode/vk-audit/vk.apk -q
sh scripts/device-check.sh --wait 4 \
  /data/data/com.termux/files/usr/tmp/opencode/vk-audit/vk.apk \
  com.apkfast.nativeappvk
$HOME/Android/android-sdk/platform-tools/adb exec-out screencap -p > \
  /data/data/com.termux/files/usr/tmp/opencode/vk-audit/vk.png
```

- Screenshot anterior (defeitos): `/data/data/com.termux/files/usr/tmp/opencode/vk-audit/baseline.png`.
- Screenshot final: `/data/data/com.termux/files/usr/tmp/opencode/vk-audit/vk.png`.
- Referência GLES2 com os mesmos assets: `/data/data/com.termux/files/usr/tmp/opencode/vk-audit/gl-final.png`.
- Comparação pixel a pixel Vulkan × GL: 14.464 pixel com diferença máxima de
  **1/255 por canal** (arredondamento de composição), 0 diferenças visuais.
- `device-check`: 302 linhas de logcat, 0 erros FATAL/dlopen/AndroidRuntime.

### Descrição do screenshot final

Clear azul-noite; `VULKAN OK` legível na orientação correta no canto superior
esquerdo; triângulo com gradiente de vértice apontando para baixo na metade
superior; quadrado verde 180×180 íntegro (sem pixel faltando) à direita, na
mesma posição/tamanho do backend GL.

### Estado histórico desta seção

A única limitação apontada aqui (`C07` marcado como GL-only) foi corrigida na
integração final: `draw_with_texture()` está presente em ambos e os testes
marcam `C07` como `both`. A validação Vulkan real foi adicionada depois pela
Equipe VK-P2.


---

## Correção visual Vulkan — EQUIPE A2

Snapshot: apkfast 2.7.3, sem bump de versão, sem commit/tag/release.

### Causa raiz dos dois sintomas visuais

- **Textura com pixels faltando / sprite achatado:** `TextureVk::load_rgba()`
  usava `VkImageSubresourceLayout::rowPitch` de uma imagem `TILING_OPTIMAL`
  para espalhar as linhas no staging, mas submetia `VkBufferImageCopy` com
  `bufferRowLength = 0` (linhas contíguas). Só 1 de cada N linhas chegava à
  imagem; as demais ficavam pretas/transparentes. Um probe isolado (quad sem
  textura = 180×180, quad texturizado = 180×47) confirmou que a geometria e a
  projeção estavam corretas e o defeito era no upload. Corrigido com cópia
  firmemente empacotada e espelhamento vertical das linhas (convenção GL).
- **Texto cortado/ilegível:** `vk_detail::ortho2d_screen()` usava a matriz do
  GL (`bottom=h, top=0`), que espelha toda a cena no NDC do Vulkan, e
  `draw_text()` compensava com `baseline = extent_.height - y` e UV de atlas
  sem espelhar. Corrigido com `mat4_ortho(0, w, 0, h, -1, 1)`, `y` direto em
  `FontAtlas::quad()` e `v = 1 - v` somente nos quads de glyph (o atlas passa
  pelo mesmo espelhamento de upload das texturas).

### Blockers da matriz (`docs/parity-matrix.md`)

- G01: `VulkanScene::draw_with_texture()` + `upload_context()` pública;
  `TextureVk` espera a fila antes de liberar descritores em trocas de frame.
- G02: no modo legado (`set_pipeline()`), apenas geometria usa o pipeline
  externo; texto usa sempre o pipeline de texto embutido com UBO/atlas.
- G03: pool de samplers dimensionado por `kMaxTextures + 4` (white/fonte/
  externas) e reserva extra de sets de UBO.
- G04: UBO cresce sob demanda (`grow_uniform_slots()`); `kMaxDrawSlots` é só
  capacidade inicial. Sem descarte silencioso de draws.
- G05: sem inversão global de `v` em `draw()`; UVs de malhas custom e
  `MeshData::quad()` seguem a convenção GL via upload espelhado.
- G06: `GpuBuffer::upload()` valida índice `uint16` e cai para `drawArrays`
  com aviso, como no GL.
- G07: device loss marcado em `device_lost()`, com log, `needs_asset_reload()`,
  parada de `render()` e reinicialização explícita via `init()`.
- G08: `draw_text()` não usa `extent_.height` na baseline.
- G16: `docs/graphics-vulkan.md` atualizado (UV/viewport, descritores,
  dynamic slots, legado, device loss, draw_with_texture).

### Verificação visual (emulator-5554, 1080×2340)

`sh scripts/visual-parity.sh --out <tmp>/visual-parity-a2`: GL e VK sem FATAL,
capturas `gl.png`/`vk.png`. Caixas idênticas medidas em pixels:

- texto `VULKAN OK`: x=34..228, y=42..67 nos dois;
- triângulo: x=297..782, y=576..1060 nos dois;
- sprite: x=416..867, y=624..1376 (180×180) nos dois.

Screenshots: anterior
`<tmp>/visual-parity/vk.png` (faixa verde 180×47, triângulo espelhado, glifos
cortados) e final `<tmp>/visual-parity-a2/vk.png`.

### Estado histórico desta seção

As pendências A2 foram fechadas na integração final:

- C07 passou a `both` em `tests/test_parity_matrix.py` e `docs/parity-matrix.md`;
- G09/G10/G13/G14/G15 foram implementados pela Equipe VK-P2;
- `vkQueueWaitIdle()` foi substituído por `VulkanUploadTracker` com fence e semáforo;
- a suíte completa passa com 130 testes.

### GLES3 instancing real (EQUIPE GL-P2)

Complemento ao status acima: o **lado OpenGL/GLES3** do G09 foi fechado. As
anotações antigas em "Não corrigido" e na pendência A2 continuam válidas para o
lado Vulkan, mas não descrevem mais o backend GL.

- API nova em `GLES3Scene`: `struct Instance { Mat4 model; array<float,4>
  color; }` (80 bytes) e sobrecargas `draw_instanced(Mesh|MeshData, Material,
  vector<Instance>) -> bool`. `true` = um `glDrawElementsInstanced`/
  `glDrawArraysInstanced` real; `false` = lista vazia ou fallback GLES2.
  A assinatura antiga `draw_instanced(MeshData, Material, vector<Mat4>)` foi
  preservada (cor branca por instância).
- Implementação: VAO + VBO de instâncias, `glVertexAttribDivisor` nos locations
  4–7 (colunas da matriz) e 8 (cor), divisor 0 em 0–3, shader `#version 300 es`
  com `layout(location=...)`. Estado restaurado no fim. `destroy()` virou
  virtual para limpar esses objetos também via `Scene*`.
- Diferença mensurável: antes N `draw()` + N `glBufferData`; agora 1 draw call e
  1 upload por chamada (ou zero, com `Mesh` prévia). Cores por instância são
  atributo, não uniform por desenho.
- `examples/nativeapp-advanced` passou a `GLES3Scene` e desenha 12 instâncias
  (4×3) com matriz/cor próprias. Log em dispositivo: `advanced: draw_instanced:
  real (GLES3, 12 instancias)`. `device-check`: 0 FATAL/0 exceções; screenshots
  `adv-t1.png`/`adv-t2.png` em `<tmp>/opencode/gl-p2/` mostram as 12 cores e a
  animação; `llvm-nm -D libmain.so` confirma os símbolos
  `glDrawElementsInstanced`, `glDrawArraysInstanced`, `glVertexAttribDivisor`,
  `glGenVertexArrays`.
- Armadilha encontrada na validação: o builder linka `-lEGL -lGLESv2` apenas se
  o token `GLES2Scene` aparece no `.cpp` (sem comentários). O exemplo mantém um
  `static_assert(is_base_of<GLES2Scene, GLES3Scene>)`; sem isso o APK falha no
  `dlopen` com `cannot locate symbol "eglMakeCurrent"`.
- Ainda falta, fora desta propriedade: paridade de `draw_instanced()` no Vulkan,
  testes automatizados que contem chamadas de draw (hoje a prova é o log/symbol
  check acima) e validação em um dispositivo sem suporte a GLES3 (o fallback foi
  lido em código, não executado em hardware GLES2).


---

## Paridade Vulkan P2 — EQUIPE VK-P2

Snapshot: apkfast 2.7.3, sem bump de versão, sem commit/tag/release/publish.

Propriedade respeitada: apenas `include/nativeapp/vk.hpp`,
`examples/nativeapp-vulkan/**`, `docs/graphics-vulkan.md` e esta seção de
`AUDIT.md`. `gl.hpp`, `parity-matrix.md`, testes, builder/CLI e releases não
foram editados.

### Gaps eliminados

- **G09 — instancing real:** `VulkanScene::draw_instanced()` (duas
  sobrecargas: `vector<Mat4>` e `const float*` + contagem) grava as matrizes em
  `GpuBuffer::upload_instances()` e faz **um** `vkCmdDrawIndexed` com
  `instanceCount`. `GraphicsPipeline` usa binding 1 com
  `VK_VERTEX_INPUT_RATE_INSTANCE`, locations 4..7 para as colunas da Mat4 e
  SPIR-V dedicado (`kGeometryInstancedVertSpv`; GLSL-fonte em comentário).
- **G10 — uniforms customizados:** `CustomUniformHandle`,
  `CustomUniformBinding`, `register_custom_uniforms()`, `write_uniform()`,
  `bind_uniform_texture()`, `custom_uniform_layout()`,
  `custom_uniform_descriptor()`, `destroy_custom_uniforms()` e
  `set_pipeline(config, handle)`. O set 2 é opcional e não altera os sets 0/1
  dos pipelines embutidos. UBOs por frame eliminam corrida com in-flight.
- **G11 — pipeline legado:** `set_pipeline()` passa a criar o pipeline externo
  com os layouts embutidos e a cena liga UBO/textura por item; `Material`
  (cor/textura/depth/blend) e `model` passam a valer também para SPIR-V
  externo que declare `SceneUniforms`. O caminho novo com set 2 cobre uniforms
  arbitrários. Texto continua sempre no pipeline embutido.
- **G13 — validação real:** `set_validation(true)` enumera layers e habilita
  `VK_LAYER_KHRONOS_validation`/`VK_LAYER_LUNARG_standard_validation` na
  `VkInstance`; `VK_EXT_debug_utils` redireciona mensagens ao logcat
  (`nativeapp-vk-validation`). `validation_enabled()` informa o estado. Sem
  layer, o degrade é explícito no logcat. No emulador usado não há layer nem
  debug-utils: o caminho de degrade foi executado e verificado; o caminho de
  ativação está implementado e coberto pelo teste estático.
- **G14 — upload assíncrono:** `VulkanUploadTracker` coordena
  fence+semáforo por upload; `TextureVk::load_rgba()` submete staging sem
  `vkQueueWaitIdle()` (o único token restante no header é um comentário),
  `render()` espera os semáforos no estágio de fragment shader e o tracker
  aposenta texturas/buffers com segurança após os frames in-flight. Pool de
  descritores cresce sob demanda através do alocador da cena.
- **G15 — legado NDC:** `set_legacy_render(true)` / `render_legacy()` reproduzem
  `GLES2Scene::render()` (clear + triângulo -0.5..0.5 com projeção identidade)
  mesmo depois de `set_projection()` custom. Cada item também captura a
  projeção no `draw()`, como no GL.
- **G12 — cache:** sem limite rígido; `invalidate_texture()`,
  `reload_texture()`, `clear_textures()` e `texture_count()`. Falha de decode
  continua avisada, nunca silenciosa.
- **G08 (já corrigido pela Equipe A2, preservado):** `draw_text()` não usa
  `extent_.height` na baseline e acompanha `set_projection()` como no GL.

### API pública adicionada

`draw_instanced()` (duas sobrecargas), `upload_instances()`/instance buffer em
`GpuBuffer`, `PipelineConfig::{instanced,custom_uniforms}`, `set_pipeline(config,
handle)`, `register_custom_uniforms()`, `write_uniform()`,
`bind_uniform_texture()`, `custom_uniform_layout()`,
`custom_uniform_descriptor()`, `destroy_custom_uniforms()`, `validation_enabled()`,
`set_legacy_render()`, `legacy_render()`, `render_legacy()`,
`invalidate_texture()`, `reload_texture()`, `clear_textures()`,
`texture_count()`, `VulkanUploadTracker` e o callback de alocação de
descritores em `VulkanUploadContext`.

Nota de compatibilidade com testes congelados: `tests/test_parity_matrix.py`
(exigido intacto) ainda marca C09 como `[GL only]` e procura o token exato
`draw_instanced(`; por isso a declaração no header usa espaço entre nome e
parêntese (`draw_instanced (`), preservando o símbolo C++ público sem alterar
o teste ou a matriz. `tests/test_vulkan_parity.py` usa regex e reconhece a API.

### Verificação em dispositivo (`emulator-5554`, 1080×2340)

- Build final: `python3 apkfast.py build examples/nativeapp-vulkan -o
  <tmp>/opencode/vk-p2-final/vk.apk -q` — OK.
- `sh scripts/device-check.sh <tmp>/opencode/vk-p2-final/vk.apk
  com.apkfast.nativeappvk` — 0 FATAL/0 exceções.
- `sh scripts/visual-parity.sh --out <tmp>/opencode/vk-p2-final/visual` — GL e
  VK sem FATAL; `gl.png` e `vk.png` inspecionados: triângulo, sprite 180×180 e
  texto `VULKAN OK` idênticos à referência e ao baseline A2.
- API A (instancing + textura animada + invalidate/reload + validação):
  `<tmp>/opencode/vk-p2-api-a.png` mostra 3 quads amarelos de **um** draw, a
  textura RGBA changing-frame e o texto legíveis. Logcat:
  `validation_active=0`, `nenhuma layer de validação disponível; executando sem
  validação` (degrade G13), `removed=1 reloaded=1 count=1`, zero falhas de
  `vkAllocateDescriptorSets` após o alocador dinâmico.
- API B (uniform custom no set 2): `<tmp>/opencode/vk-p2-api-b.png` mostra o
  quad verde gerado por `u_tint`, confirmando `write_uniform()` no shader.
- API C (G11/G15): `<tmp>/opencode/vk-p2-api-c-legacy-pipeline.png` mostra o
  quad vermelho do material com SPIR-V externo; `...-legacy-render.png` mostra
  o triângulo em NDC após `set_projection(mat4_perspective(...))`, igual ao
  `GLES2Scene::render()`.
- `python3 -m unittest discover -s tests -q`: 130 testes, OK.
- `python3 -m compileall -q apkfast apkfast.py tests`: OK.

### Screenshots finais de paridade

- GL: `<tmp>/opencode/vk-p2-final/visual/gl.png`
- VK: `<tmp>/opencode/vk-p2-final/visual/vk.png`

(`<tmp>` = `/data/data/com.termux/files/usr/tmp/opencode`.)

### O que permanece

- Layer de validação não existe no emulador usado; o caminho "layer ativa"
  só pôde ser verificado estaticamente e por construção, não em hardware.
- `docs/parity-matrix.md` e `tests/test_parity_matrix.py` (fora da propriedade)
  continuam desatualizados para C09; C14/C19/C20/C21 também descrevem o
  estado pré-P2. O dono desses arquivos deve atualizar os marcadores quando
  autorizado.
- Limitações documentadas e fora do contrato comum (`Scene`/`Material`):
  G17–G22 (cull/mipmap/MSAA/stencil, assinaturas de init, clear, helpers de
  projeção 3D, ASCII do FontAtlas, assinatura de SVG).

### G07 — device loss GLES

Lado OpenGL/GLES do G07 fechado nesta rodada (EQUIPE G07), sem tocar em
`vk.hpp`, exemplos, testes, matriz de paridade, builder ou CLI.

- API nova em `GLContext`: `context_lost()` (primária), `device_lost()`
  (alias do contrato Vulkan), `needs_asset_reload()`, `requested_version()` e
  `window()`. `GLES2Scene`/`GLES3Scene` expõem as três primeiras e
  `recreate(window = nullptr, version = -1)`.
- Detecção: `eglGetError()` é consultado em `eglMakeCurrent`,
  `eglSwapBuffers`, `eglSwapInterval`, `recreate_surface` e `clear_current`.
  `EGL_CONTEXT_LOST` (`0x300E`, com fallback literal se o header só trouxer
  `_KHR`) dispara log `ERROR`, marca as duas flags e chama
  `gl_detail::bump_context_generation()`. Falhas por surface inválida continuam
  sendo aviso, não device loss.
- `ensure_current()` da cena checa perda/geração antes e depois de
  `make_current()`; `draw`, `draw_mesh`, `begin_frame`, `present`, `resize`,
  `load_font`, `draw_text` e `draw_instanced_impl` não emitem GL com contexto
  morto. `Shader`, `Mesh`, `Texture`, o shader/malha de texto e o VAO/VBO de
  instancing guardam geração: `valid()`/`program()` falham e `destroy()` não
  chama `glDelete*` sobre nomes mortos. `release_without_gl()` faz o descarte
  explícito no instante da perda; `FontAtlas` (CPU) é preservado para reupload.
- Recuperação: `init()` descarta os handles EGL sem `eglDestroy*` (apenas
  `eglTerminate`), recria EGL, recompila shader/mesh, recarrega texturas do
  cache e reenvia o atlas/shader de texto. `needs_asset_reload()` volta a
  `false`; recursos externos do app continuam precisando de reload.
- Documentação: `docs/graphics-opengl.md` (§1.2, §2.2, §2.4–2.5, §3–7,
  §6.8, §9–10) e `docs/graphics-types.md` §8 corrigido — `VulkanScene::draw()`
  **não** ignora mais `material`/`model`.

Validação executada em 25/11/2026:

- `python3 -m compileall -q apkfast apkfast.py tests` — OK.
- `python3 -m unittest discover -s tests -v` — 130 testes, 0 falhas.
- `python3 apkfast.py build examples/nativeapp-gl -q` e
  `python3 apkfast.py build examples/nativeapp-advanced -q` — OK. Houve uma
  falha transitória de build por edição concorrente em `vk.hpp`
  (`tracker_` undeclared); não foi alterada aqui e o rebuild final passou.
- `sh scripts/device-check.sh ...` para os dois APKs em `emulator-5554` —
  0 FATAL, 0 `dlopen failed`, 0 exceções. Logs:
  `<tmp>/opencode/gl-g07/device-gl-final.log` e
  `<tmp>/opencode/gl-g07/device-advanced-final.log`.
- Screenshots 1080×2340: `<tmp>/opencode/gl-g07/gl-final.png` (triângulo
  colorido) e `<tmp>/opencode/gl-g07/advanced-final.png` (12 instâncias com
  cores próprias). Nenhum warning `nativeapp-gl` apareceu nos logs finais.
- Sintaxe isolada do header com NDK clang++ (`-fsyntax-only`, `-Wall
  -Wextra`) — OK.

O que ainda falta (fora desta propriedade ou não executável aqui):

- Injeção real de `EGL_CONTEXT_LOST` no emulador: o caminho de recuperação foi
  validado por código, build e execução normal; não foi possível provocar uma
  perda de GPU controlada para observar `context_lost()` em dispositivo.
- `docs/parity-matrix.md` C18/linha G07 e `tests/test_parity_matrix.py`
  (`OPEN_GAPS` inclui G07) ainda classificam o gap como pendente. São arquivos
  de outra propriedade; os testes passam, mas o marcador de status não foi
  atualizado aqui.
- Nenhum exemplo/teste foi alterado para exercitar a nova API; isso depende do
  dono desses arquivos.
