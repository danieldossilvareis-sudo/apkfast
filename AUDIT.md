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

## Não corrigido (aceito / fora de escopo)
- `stb_image.h` (terceirizado) continua com `STB_IMAGE_IMPLEMENTATION` no header
  público — gera código por TU, mas funciona e é opt-in implícito
- `GLES3Scene::draw_instanced()` ainda usa loop de uniforms em vez de
  `glDrawArraysInstanced` (compatibilidade GLES2)
- AAudio é compile-time (`__ANDROID_API__ >= 26`); com minSdk 21 em device 26+
  cai para OpenSLES — correção exigiria `dlopen/dlsym`
- Race conditions com builds paralelos no mesmo diretório (baixa probabilidade)
- Algumas checagens JNI adicionais em casos extremos de exceção pendente
