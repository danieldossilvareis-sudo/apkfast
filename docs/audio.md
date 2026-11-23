# Áudio (`nativeapp::Audio` / `audio.hpp`)

> Guia completo do helper de áudio do apkfast.
>
> Fontes consultadas: `include/nativeapp/audio.hpp` (API e implementação),
> `include/nativeapp/assets.hpp` (leitura de `assets/`),
> `examples/device-demo/jni/main.cpp` (exemplo funcional).
> Complementos de infraestrutura consultados: `apkfast/builder.py` (linkagem e
> empacotamento de assets), `include/nativeapp/app.hpp` (injeção do
> `AAssetManager`), `include/nativeapp/nativeapp.hpp` (header agregador).
>
> **Regra deste documento:** só está aqui o que existe no código atual. Quando o
> código não faz algo — ou faz algo diferente do que o comentário sugere — isso
> é dito explicitamente (veja §11 e §12).

## Índice

1. [Visão geral](#1-visão-geral)
2. [Requisitos de build e linkagem](#2-requisitos-de-build-e-linkagem)
3. [`AudioBuffer`: o dado que você toca](#3-audiobuffer-o-dado-que-você-toca)
4. [Singleton e inicialização (`init`)](#4-singleton-e-inicialização-init)
5. [Carregando WAV de assets (`load_wav`)](#5-carregando-wav-de-assets-load_wav)
6. [Reprodução (`play`) e volume nos dois backends](#6-reprodução-play-e-volume-nos-dois-backends)
7. [Volume, estado e limpeza](#7-volume-estado-e-limpeza)
8. [Tabela completa de métodos públicos](#8-tabela-completa-de-métodos-públicos)
9. [Exemplo completo: beep em memória + WAV de assets](#9-exemplo-completo-beep-em-memória--wav-de-assets)
10. [Gerando um WAV de teste com Python](#10-gerando-um-wav-de-teste-com-python)
11. [Limitações reais (o que NÃO existe)](#11-limitações-reais-o-que-não-existe)
12. [Armadilhas e diagnóstico](#12-armadilhas-e-diagnóstico)
13. [Resumo rápido](#13-resumo-rápido)

---

## 1. Visão geral

`nativeapp::Audio` é um helper **pequeno e direto**: ele inicializa um backend de
saída, lê um arquivo **WAV PCM 16-bit** de `assets/` e envia as amostras para o
dispositivo uma vez. Não há motor de áudio, mixer nem fila de eventos. A
decodificação de formatos comprimidos vive em `Audio::load()` (`codecs.hpp`,
§8) — `load_wav()` continua sendo só PCM.

```
                       ┌──────────────────────────────────────┐
                       │  Audio::instance()  (singleton)      │
                       └───────────────┬──────────────────────┘
                                       │ init()
                    ┌──────────────────┴──────────────────┐
                    │ #if __ANDROID_API__ >= 26           │
                    ▼                                     ▼
        ┌───────────────────────┐             ┌───────────────────────┐
        │ AAudio (API 26+)      │  falha      │ OpenSLES (fallback)   │
        │ PCM_I16 / 44100 / 2ch │ ──────────► │ engine + output mix   │
        │ write() bloqueante    │             │ (player criado depois)│
        └───────────────────────┘             └───────────────────────┘
                    ▲                                     ▲
                    └──────────────┬──────────────────────┘
                                   │ play(buffer, volume)
                   ┌───────────────┴────────────────┐
                   │ load_wav("efeito.wav", buf)    │
                   │   → Assets::read()             │
                   │   → AAssetManager (AASSET_MODE_BUFFER)
                   └────────────────────────────────┘
```

Pontos importantes já de partida:

- O cabeçalho diz "AAudio (API 26+) com fallback OpenSLES" e "WAV PCM de
  assets/". É exatamente isso que o código implementa.
- A escolha do backend é **compile-time** (`#if __ANDROID_API__ >= 26`), com uma
  tentativa de AAudio em runtime apenas quando esse bloco foi compilado.
- `play()` reproduz **um buffer por chamada**, sem loop e sem callback de fim.
- O ganho final de cada reprodução é
  `clamp01(volume passado a play()) × clamp01(volume() guardado)`:
  **`set_volume()` tem efeito real** no código dos dois backends (na prática,
  apenas no caminho AAudio, que é o que toca — §6.3 e §7).
- O comentário de abertura do header menciona "controle de volume/loop", mas
  **não existe nenhuma API de loop** no código (§11).

---

## 2. Requisitos de build e linkagem

Você não precisa configurar nada manualmente se usa o builder do apkfast. A
linkagem é feita em `apkfast/builder.py` (por volta das linhas 338–349):

| Biblioteca / flag | Quando é adicionada | Por quê |
|---|---|---|
| `-llog` | sempre | `Audio::init()` chama `__android_log_print` (tag `nativeapp-audio`) |
| `-landroid`, `-lm`, `-ldl` | sempre | dependências base do `.so` nativo |
| `-Wl,--no-as-needed -lOpenSLES` | **sempre** | OpenSLES é o fallback; `--no-as-needed` evita o linker descartá-lo |
| `-laaudio` | **somente se `tc.api >= 26`** | o stub `libaaudio.so` não existe em NDK API 21–25 |
| `-Wl,--as-needed` | logo depois | restaura o comportamento padrão para as libs seguintes |

Trecho real do builder:

```python
cmd += [
    *objects,
    "-lm",
    "-ldl",
    "-landroid",
    "-llog",
    "-Wl,--no-as-needed", "-lOpenSLES",
    *( ["-laaudio"] if tc.api >= 26 else [] ),
    "-Wl,--as-needed",
    ...
]
```

### `--api` × `--min-sdk`

| Opção | Default | Afeta | Relação com áudio |
|---|---|---|---|
| `--api` (CLI) | `36` | API usada pelo NDK/clang → define `__ANDROID_API__` | **É o que liga/desliga o bloco AAudio** |
| `--min-sdk` (CLI/manifesto) | `21` | `android:minSdkVersion` do APK | Não muda `__ANDROID_API__`; apenas declara compatibilidade |

Consequência prática: com a combinação comum `minSdk 21` + build `--api 21`, um
dispositivo com Android 26+ **ainda executa o caminho OpenSLES**, porque o código
AAudio nunca foi compilado. Com `--api >= 26`, o bloco AAudio existe e é tentado
primeiro; se `init_aaudio()` falhar, o código loga e tenta OpenSLES. O `CHANGELOG.md`
registra que o build com `--api 21` (sem `-laaudio`) foi validado.

> **Sem `dlopen`/`dlsym`:** não há carregamento opcional de `libaaudio.so` em
> runtime. O `AUDIT.md` registra essa limitação e aponta que uma correção
> exigiria `dlopen/dlsym`.

### Assets

O builder empacota o diretório `<projeto>/assets` com `-A` (`builder.py:566-568`):

```python
assets = options.assets_dir or (options.project / "assets")
if assets.is_dir():
    cmd += ["-A", str(assets)]
```

Os arquivos ficam na **raiz** do `AAssetManager`. Portanto um arquivo em
`<projeto>/assets/efeito.wav` é aberto como `"efeito.wav"` (veja a armadilha em
§12.1 sobre o prefixo `assets/`).

Inclua o helper com o header agregador ou direto:

```cpp
#include <nativeapp/nativeapp.hpp>  // inclui audio.hpp
// ou
#include <nativeapp/audio.hpp>
```

---

## 3. `AudioBuffer`: o dado que você toca

Definição literal (`audio.hpp:24-30`):

```cpp
struct AudioBuffer {
    std::vector<int16_t> samples;
    int32_t sample_rate = 44100;
    int channels = 1;

    bool empty() const { return samples.empty(); }
};
```

| Campo | Tipo | Default | Significado no código |
|---|---|---|---|
| `samples` | `std::vector<int16_t>` | vazio | Amostras PCM 16-bit **interleaved** (L, R, L, R…) quando `channels == 2` |
| `sample_rate` | `int32_t` | `44100` | Taxa em Hz. Usada no formato OpenSLES (`sample_rate * 1000`, miliHz) e como **origem do resample no AAudio** (`sample_rate → 44100`) |
| `channels` | `int` | `1` | `1` = mono, `2` = estéreo. Usado para calcular frames, duplicar mono em L/R e montar a máscara de speakers |
| `empty()` | `bool` | — | Retorna `samples.empty()`; faz parte da validação de `play()` (§6) |

Montando um buffer manualmente:

```cpp
nativeapp::AudioBuffer buf;
buf.sample_rate = 44100;
buf.channels = 1;       // use 1 ou 2
buf.samples.assign(4410, 0);  // 0,1 s de silêncio
```

> `AudioBuffer` **não valida** nada por conta própria, mas `play()` valida o
> buffer antes de despachar (canais 1–2, `sample_rate > 0` e número de amostras
> múltiplo de `channels`). Um buffer inválido é silenciosamente ignorado;
> `load_wav()` já garante 1–2 canais.

---

## 4. Singleton e inicialização (`init`)

### Singleton

```cpp
static Audio& instance() {
    static Audio singleton;
    return singleton;
}
```

Há uma única instância por processo. O construtor é privado; o destrutor chama
`destroy()` (§7).

### Fluxo de `init()` (`audio.hpp:41-51`)

```cpp
bool init() {
    if (initialized_) return true;
#if __ANDROID_API__ >= 26
    if (init_aaudio()) { initialized_ = true; return true; }
    __android_log_print(ANDROID_LOG_INFO, "nativeapp-audio",
                        "AAudio indisponivel, usando OpenSLES");
#endif
    if (init_opensles()) { initialized_ = true; return true; }
    return false;
}
```

| Situação | Resultado |
|---|---|
| Já inicializado | `true` imediato (idempotente) |
| `--api >= 26` e `init_aaudio()` OK | `true`, `using_aaudio() == true` |
| `--api >= 26` e AAudio falha, OpenSLES OK | `true`, log INFO com tag `nativeapp-audio`, `using_aaudio() == false` |
| `--api <= 25` | Bloco AAudio nem existe; tenta só OpenSLES |
| OpenSLES falha | `false`, **sem log** |

### Configuração pedida ao AAudio (`init_aaudio`)

| Chamada | Valor |
|---|---|
| `AAudioStreamBuilder_setDirection` | `AAUDIO_DIRECTION_OUTPUT` |
| `AAudioStreamBuilder_setSharingMode` | `AAUDIO_SHARING_MODE_SHARED` |
| `AAudioStreamBuilder_setPerformanceMode` | `AAUDIO_PERFORMANCE_MODE_LOW_LATENCY` |
| `AAudioStreamBuilder_setFormat` | `AAUDIO_FORMAT_PCM_I16` |
| `AAudioStreamBuilder_setChannelCount` | `2` |
| `AAudioStreamBuilder_setSampleRate` | `44100` |
| `AAudioStreamBuilder_openStream` | guarda em `aaudio_stream_` |

O builder é sempre apagado com `AAudioStreamBuilder_delete()`. A função verifica
**todos os retornos**: `AAudio_createStreamBuilder()` precisa devolver
`AAUDIO_OK` **e** um builder não nulo, e `openStream()` precisa devolver
`AAUDIO_OK` **e** um stream não nulo (se o stream existir apesar do erro, ele é
fechado). São valores *solicitados* — o código **não chama**
`AAudioStream_getSampleRate()` nem `getChannelCount()` para confirmar o que o
dispositivo negociou (§11).

### Configuração do OpenSLES (`init_opensles`)

Cada etapa verifica o retorno e, em caso de falha, chama `destroy_opensles()` e
devolve `false`:

1. `slCreateEngine()`;
2. `Realize()` no engine;
3. `GetInterface(SL_IID_ENGINE)` (com checagem de ponteiro);
4. `CreateOutputMix()` pedindo `SL_IID_PLAY` (com checagem de ponteiro);
5. `Realize()` no output mix;
6. retorna `true`.

Assim, **`init()` verifica erros nos dois backends**: uma falha parcial não
deixa objetos OpenSLES órfãos e `initialized_` só vira `true` quando o backend
está realmente pronto. A única falha ainda sem log é a do OpenSLES (§12.4).

**O player não é criado aqui**: `sles_player_` continua nulo até
`play_opensles()` — o que tem uma consequência importante (§6.2 e §11).

Uso típico (faça em `on_ready()`, nunca sem verificar o retorno):

```cpp
const bool ok = nativeapp::Audio::instance().init();
LOGI("audio init=%d aaudio=%d", ok,
     nativeapp::Audio::instance().using_aaudio());
```

---

## 5. Carregando WAV de assets (`load_wav`)

```cpp
bool load_wav(const std::string& path, AudioBuffer& out) const;
```

### Caminho percorrido

1. `auto& assets = Assets::instance(nullptr);` — reusa o singleton já criado
   pelo `App` (§12.1). Se `assets.manager()` for nulo, retorna `false`.
2. `assets.read(path)` → `AAssetManager_open(manager_, path, AASSET_MODE_BUFFER)`,
   lê o arquivo inteiro para `std::vector<uint8_t>`.
3. Menos de 44 bytes → `false`.
4. `RIFF` no offset 0 **e** `WAVE` no offset 8 → senão `false`.
5. Varredura de chunks a partir do offset 12.

### Varredura de chunks

Para cada chunk em `offset`:

| Passo | Código / efeito |
|---|---|
| ID | 4 bytes em `offset` |
| Tamanho | `le32(offset + 4)` (little-endian explícito) |
| Validação | `chunk_size > data.size() - offset - 8` → `false` (evita OOB e wrap) |
| `fmt ` | Só é lido se `chunk_size >= 16`: `audio_format` (+0), `num_channels` (+2), `sample_rate` (+4), `bits_per_sample` (+14) |
| `data` | `pcm.assign(payload, payload + chunk_size)` |
| Avanço | `8 + chunk_size + (chunk_size & 1)` — respeita padding ímpar |

O loop aceita chunks em qualquer ordem (`data` antes de `fmt ` funciona).
Chunks desconhecidos são ignorados. Se houver mais de um `data`, o **último**
sobrescreve os anteriores. O campo de tamanho do RIFF (offset 4) não é validado.

### Validação final (`audio.hpp:91-95`)

| Condição | Motivo |
|---|---|
| `audio_format == 1` | apenas PCM (`WAVE_FORMAT_PCM`) |
| `bits_per_sample == 16` | casa com `std::vector<int16_t>` |
| `!pcm.empty()` | nada para tocar |
| `sample_rate != 0` | taxa inválida |
| `1 <= num_channels <= 2` | mono ou estéreo apenas |

Em caso de sucesso:

```cpp
out.sample_rate = static_cast<int32_t>(sample_rate);
out.channels = num_channels;
out.samples.resize(pcm.size() / 2);
std::memcpy(out.samples.data(), pcm.data(), out.samples.size() * 2);
```

> `out` **só é modificado no sucesso**: qualquer falha retorna `false` sem
> tocar nos campos do buffer de saída. Se `pcm` tiver tamanho ímpar, o último
> byte é silenciosamente descartado (`pcm.size() / 2`).

Uso:

```cpp
nativeapp::AudioBuffer efeito;
if (!nativeapp::Audio::instance().load_wav("efeito.wav", efeito)) {
    LOGI("WAV invalido ou ausente");
} else {
    LOGI("WAV: %d Hz, %d ch, %zu amostras",
         efeito.sample_rate, efeito.channels, efeito.samples.size());
}
```

> O `README.md` de `include/nativeapp/` mostra
> `load_wav("assets/efeito.wav", buffer)`, mas o código repassa a string
> diretamente ao `AAssetManager`, cujo caminho é relativo à raiz de `assets/`.
> Use `"efeito.wav"`; `"assets/efeito.wav"` só encontraria o arquivo se ele
> estivesse em `assets/assets/efeito.wav`. Veja §12.1.

---

## 6. Reprodução (`play`) e volume nos dois backends

```cpp
void play(const AudioBuffer& buffer, float volume = 1.0f);
```

`play()` valida o buffer e depois despacha:

```cpp
if (buffer.empty() || buffer.channels < 1 || buffer.channels > 2 ||
    buffer.sample_rate <= 0 ||
    buffer.samples.size() % static_cast<size_t>(buffer.channels) != 0) {
    return;
}
```

| Estado | Ação |
|---|---|
| `aaudio_stream_ != nullptr` | `play_aaudio(buffer, volume)` |
| senão, `sles_player_ != nullptr` | `play_opensles(buffer, volume)` |
| senão (init não chamado / falhou) | **nada** (silencioso) |

O ganho aplicado pelos dois backends vem de
`gain_for(volume) = clamp01(requested) * clamp01(volume_)`, com clamp também em
`scale_sample()` (`-32768..32767`). Portanto `play(buf, 2.0f)` não estoura
`int16_t` e `set_volume()` participa do resultado (§6.3).

### 6.1 Caminho AAudio (`play_aaudio`)

```
frames = buffer.samples.size() / buffer.channels
step   = double(buffer.sample_rate) / 44100.0
out_frames = ceil(frames / step)
```

O stream é fixo em **44100 Hz / 2 canais / PCM_I16**, mas o buffer de origem não
precisa mais ter essa taxa: o código faz **resample linear** de
`buffer.sample_rate` para 44100 e converte mono para estéreo no mesmo laço.

| Aspecto | O que acontece |
|---|---|
| `buffer.channels == 1` | Cada amostra de origem é lida uma vez (`src_ch = 0`) e gravada em L **e** R |
| `buffer.channels == 2` | Cada canal lê a sua própria amostra (`src_ch = ch`) |
| Amostragem | `position = i * step`; `idx = floor(position)`, `next = idx+1` (ou `idx` no último frame), `t = position - idx`; interpola `a + (b-a)*t` |
| Exemplo | WAV **22050 Hz mono** → 44100 Hz estéreo (cada amostra vira duas, com interpolação) |
| Ganho | `scale_sample(valor, gain_for(volume))` em cada amostra de saída |
| Proteções | `out_frames > 0` e `<= SIZE_MAX/2`; `idx` é limitado a `frames-1` |

Depois: `AAudioStream_requestStart()` aceita `AAUDIO_OK` **ou**
`AAUDIO_ERROR_INVALID_STATE` (qualquer outro erro aborta a escrita) e um laço de
`AAudioStream_write(stream, frame, remaining, 1000000000LL)` — **1 segundo de
timeout em nanossegundos por chamada**. O ponteiro avança `written * 2`
int16-t por frame escrito; `written <= 0` encerra o laço. Escritas parciais são
completadas pelo laço; erros de escrita não são relatados.

É uma escrita **bloqueante na thread chamadora**: não há callback, não há fila
assíncrona (§11).

### 6.2 Caminho OpenSLES (`play_opensles`)

Configuração montada por chamada:

| Item | Valor |
|---|---|
| Source locator | `SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE`, 1 buffer |
| Formato | `SL_DATAFORMAT_PCM`, canais do buffer, `sample_rate * 1000` (miliHz), 16-bit fixo, little-endian |
| Speakers | `FRONT_LEFT \| FRONT_RIGHT` se 2 canais; `FRONT_CENTER` se 1 |
| Sink locator | `SLDataLocator_OutputMix{SL_DATALOCATOR_OUTPUTMIX, sles_output_mix_}` — uma **struct**, não ponteiro do objeto (comentário explícito no código) |
| Interfaces pedidas | `SL_IID_PLAY`, `SL_IID_BUFFERQUEUE`, `SL_IID_VOLUME` |

Fluxo: copia `samples` para `sles_samples_` (mantido vivo até o player
terminar) → se `gain < 1.0f`, escala cada amostra dessa cópia com
`scale_sample(sample, gain_for(volume))` → destrói player anterior (se houver) →
`CreateAudioPlayer()` (falha = retorno silencioso) → `Realize()` → pega
`SLPlayItf` e `SLAndroidSimpleBufferQueueItf` →
`Enqueue(sles_samples_.data(), size)` → `SetPlayState(SL_PLAYSTATE_PLAYING)`. Um
único buffer por player; não há re-enqueue nem callback de fim. O buffer original
`buffer.samples` **não é alterado**.

> **Armadilha séria aqui:** **`play()` nunca chega nesse código.** Ele só chama
> `play_opensles()` se `sles_player_` já não for nulo, mas `sles_player_` só é
> atribuído **dentro** de `play_opensles()`. Com o fallback OpenSLES, `play()` é
> um no-op permanente, mesmo com o ganho já implementado. Isso está detalhado em
> §11. A interface `SL_IID_VOLUME` continua sendo solicitada mas nunca obtida —
> o volume é aplicado por escala de amostras, não por `SetVolumeLevel`.

### 6.3 Resumo do volume (o que realmente acontece)

| Entrada | AAudio | OpenSLES (se o caminho fosse alcançado) |
|---|---|---|
| `play(buf, v)` | Ganho `clamp01(v) × clamp01(volume_)`, amostra limitada a `int16_t` | Mesmo ganho, aplicado a uma cópia de `samples` |
| `set_volume(v)` | Afeta o ganho da **próxima** chamada de `play()` | Idem |
| `volume()` | Devolve o valor clampado guardado em `volume_` | Idem |
| `v` NaN / ∞ em `set_volume()` | `clamp01()` devolve `1.0f` (volume default) | Idem |
| Som já em reprodução | Não é alterado: o ganho é calculado dentro de `play()` | Não é alterado |

O ganho é **por reprodução**: chame `set_volume()` antes de `play()`. Como o
caminho OpenSLES é inalcançável no código atual (§6.2), na prática o ajuste só
surte efeito em builds com AAudio disponível.

---

## 7. Volume, estado e limpeza

| Método | Comportamento exato |
|---|---|
| `void set_volume(float volume)` | `volume_ = std::isfinite(volume) ? std::max(0, std::min(1, volume)) : 1.0f`; NaN/∞ voltam ao default `1.0f` |
| `float volume() const` | Retorna `volume_` (default `1.0f`) |
| `bool using_aaudio() const` | `aaudio_stream_ != nullptr` — **não** significa "API >= 26" |
| `void destroy()` | **Idempotente**: sem stream/objetos, só zera `initialized_` e limpa `sles_samples_`. Caso contrário, AAudio: `requestStop` + `close` + zera ponteiro; OpenSLES: `Destroy` em player → output mix → engine, zera os ponteiros e a interface do engine. O retorno de `requestStop` é ignorado |
| `~Audio()` | Chama `destroy()` automaticamente (destrutor do singleton) |

Depois de `destroy()`, os ponteiros de stream/player/mix/engine e
`sles_engine_interface_` ficam nulos, `sles_samples_` é esvaziado e
`initialized_` volta a `false`; para voltar a tocar, chame `init()` de novo.
Chamar `destroy()` duas vezes (ou pelo destrutor depois de uma chamada manual) é
seguro: não há `Destroy` em ponteiro nulo.

`set_volume()` multiplica o ganho da **próxima** chamada de `play()` em ambos os
backends (§6.3); ele não altera som já em reprodução.

---

## 8. Tabela completa de métodos públicos

| Método | Assinatura | Retorno / efeito | Observações |
|---|---|---|---|
| `instance` | `static Audio& instance()` | Singleton | Construtor privado; uma instância por processo |
| `init` | `bool init()` | `true` se backend pronto | Idempotente; AAudio é compile-time; checa erros nos dois backends (falha OpenSLES sem log) |
| `load_wav` | `bool load_wav(const std::string&, AudioBuffer&) const` | Preenche `out` ou retorna `false` | PCM 16-bit, mono/estéreo, WAV inteiro em memória |
| `load` | `bool load(const std::string&, AudioBuffer&) const` | Preenche `out` ou retorna `false` | Implementado em `codecs.hpp`; aceita `.wav`/`.ogg`/`.mp3` |
| `play` | `void play(const AudioBuffer&, float volume = 1.0f)` | Toca uma vez | Valida buffer; AAudio faz resample para 44100/2ch e é bloqueante; fallback OpenSLES inalcançável |
| `set_volume` | `void set_volume(float)` | Clampa e guarda 0..1 | **Multiplica o ganho** de `play()`; NaN/∞ ⇒ `1.0f` |
| `volume` | `float volume() const` | Valor guardado | Default `1.0f` |
| `using_aaudio` | `bool using_aaudio() const` | `aaudio_stream_ != nullptr` | `'false'` pode significar fallback **ou** init falhado |
| `destroy` | `void destroy()` | Libera stream/objetos | Idempotente; chamado também pelo destrutor |
| `~Audio` | `~Audio()` | Chama `destroy()` | — |

Métodos que **não existem** (para evitar buscas frustradas): `pause`, `resume`,
`stop`, `loop`, `is_playing`, `seek`, `play_asset`, `set_callback`, `mix`,
`load_ogg`, `load_mp3`. Para OGG/MP3 use `load()` (§8).

---

## 9. Exemplo completo: beep em memória + WAV de assets

Baseado em `examples/device-demo/jni/main.cpp`, com o carregamento de WAV
acrescentado. Coloque o WAV em `<projeto>/assets/efeito.wav`.

```cpp
// jni/main.cpp — áudio: beep sintético + WAV de assets
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#include <cmath>
#include <vector>

#define LOG_TAG "audio-demo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class AudioDemo : public App {
public:
    explicit AudioDemo(android_app* app) : App(app) {}

private:
    void on_ready() override {
        // O App já injetou o AAssetManager em Assets antes de on_ready().
        const bool ok = Audio::instance().init();
        const bool aaudio = Audio::instance().using_aaudio();
        LOGI("audio init=%d aaudio=%d", ok, aaudio);
        // (!) Com o fallback OpenSLES, play() é no-op nesta versão do código.
        Audio::instance().set_volume(0.8f);  // ganho global da próxima play()

        // --- 1) Beep 440 Hz gerado em memória (0,15 s, mono) -------------
        beep_.sample_rate = 44100;
        beep_.channels = 1;
        const int frames = 44100 * 15 / 100;
        beep_.samples.resize(frames);
        for (int i = 0; i < frames; ++i) {
            const float envelope = 1.0f - static_cast<float>(i) / frames;
            beep_.samples[i] = static_cast<int16_t>(
                std::sin(2.0f * 3.14159265f * 440.0f * i / 44100.0f) *
                12000.0f * envelope);
        }
        Audio::instance().play(beep_);           // volume default = 1.0

        // --- 2) WAV PCM 16-bit vindo de assets/ --------------------------
        // Caminho relativo à raiz de assets/ (arquivo: assets/efeito.wav).
        if (Audio::instance().load_wav("efeito.wav", effect_)) {
            LOGI("efeito.wav: %d Hz, %d canais, %zu amostras",
                 effect_.sample_rate, effect_.channels, effect_.samples.size());
            // ganho final = 0.5 (play) * 0.8 (set_volume) = 0.4.
            Audio::instance().play(effect_, 0.5f);
        } else {
            LOGI("efeito.wav ausente ou invalido");
        }
    }

    void on_touch(TouchAction action,
                  const std::vector<TouchPoint>& pts) override {
        if (action == TouchAction::Down && !pts.empty()) {
            Audio::instance().play(beep_);
        }
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == 4) request_exit();  // BACK
    }

    AudioBuffer beep_;
    AudioBuffer effect_;
};

APP_MAIN(AudioDemo)
```

Diferenças em relação ao `device-demo` original:

- `init()` e `using_aaudio()` são chamados em **instruções separadas**. No
  exemplo do repositório eles aparecem como argumentos de um único `LOGI`, e a
  ordem de avaliação de argumentos em C++ não é garantida — o valor de
  `using_aaudio()` pode ser lido **antes** de `init()` (§12.4).
- O WAV é carregado com `"efeito.wav"`, não `"assets/efeito.wav"`.

> `on_ready()` roda uma vez, após a primeira janela válida. Chamar `play()` em
> `on_draw()` a cada frame não toca um loop: dispara uma reprodução nova por
> frame e pode bloquear a thread de render (§12.2).

---

## 10. Gerando um WAV de teste com Python

O módulo `wave` da biblioteca padrão gera exatamente o formato aceito pelo
parser: RIFF/WAVE, `audio_format = 1`, 16-bit little-endian, 44100 Hz, mono.

```python
# make_wav.py — cria assets/efeito.wav (beep 440 Hz, 0,15 s)
import math
import struct
import wave
from pathlib import Path

RATE = 44100
SECONDS = 0.15
FRAMES = int(RATE * SECONDS)

out = Path("assets/efeito.wav")
out.parent.mkdir(parents=True, exist_ok=True)

with wave.open(str(out), "wb") as w:
    w.setnchannels(1)          # 1 ou 2 — o helper aceita apenas estes
    w.setsampwidth(2)          # 16-bit
    w.setframerate(RATE)       # qualquer taxa > 0; AAudio pede 44100
    data = bytearray()
    for i in range(FRAMES):
        envelope = 1.0 - i / FRAMES
        value = 12000.0 * math.sin(2.0 * math.pi * 440.0 * i / RATE) * envelope
        data += struct.pack("<h", int(value))
    w.writeframes(data)

print(f"gerado: {out} ({FRAMES} frames)")
```

Execução:

```bash
python3 make_wav.py
file assets/efeito.wav
# RIFF (little-endian) data, WAVE audio, Microsoft PCM, 16 bit, mono 44100 Hz
```

Para estéreo, troque `w.setnchannels(2)` e intercale as amostras em pares
`struct.pack("<hh", left, right)`. O parser do helper aceita 1 ou 2 canais e
rejeita qualquer outro formato (inclusive `WAVE_FORMAT_EXTENSIBLE`, §11).

---

## 11. Limitações reais (o que NÃO existe)

| Limitação | O que o código faz / deixa de fazer |
|---|---|
| **AAudio é compile-time** | `#if __ANDROID_API__ >= 26` em `init()`, `init_aaudio()` e `play_aaudio()`. Com `--api 21` (`minSdk 21`), um dispositivo 26+ cai para OpenSLES. Não há checagem de API em runtime nem `dlopen/dlsym` |
| **Fallback OpenSLES não reproduz** | `play()` exige `sles_player_ != nullptr`, mas esse ponteiro só é preenchido dentro de `play_opensles()`. Caminho inalcançável: `init()` pode retornar `true` e `play()` continuar sem efeito |
| **Sem streaming** | `Assets::read()` carrega o WAV inteiro em `std::vector<uint8_t>`; depois tudo vira `std::vector<int16_t>`. Sem `read` incremental, sem ring buffer, sem callback de underrun |
| **Sem loop** | Apesar de "loop" no comentário do topo do header, `play()` toca um buffer uma vez e não há nenhum método de repetição |
| **OGG/MP3 só em `load()`** | `load_wav()` exige `audio_format == 1` (PCM); `Audio::load()` em `codecs.hpp` faz a decodificação de `.ogg`/`.mp3` |
| **Sem WAVE_FORMAT_EXTENSIBLE** | `0xFFFE` cai na validação `audio_format != 1` → `load_wav()` retorna `false` |
| **AAudio com formato de saída fixo** | PCM_I16, 44100 Hz, 2 canais sempre. A entrada é resampleada linearmente para 44100 e mono é duplicado em L/R; valores negociados pelo stream não são consultados |
| **Escrita bloqueante** | `AAudioStream_write(..., 1000000000LL)` com timeout de 1 s, em chamada síncrona; nenhum callback assíncrono |
| **Ganho por reprodução** | `set_volume()` vale para a **próxima** `play()`; não existe controle de volume do stream em tempo real. `play()` no OpenSLES permanece inalcançável (§6.2) |
| **Falhas ainda silenciosas** | `init_opensles()` e `init_aaudio()` agora verificam os retornos, mas `Enqueue`, `SetPlayState` e escritas AAudio com `written <= 0` apenas encerram/retornam sem log; `init()` não loga falha OpenSLES |
| **Sem concorrência explícita** | Um único stream AAudio e um único player OpenSLES; nenhum mutex ou controle de múltiplas reproduções sobrepostas |
| **Sem pause/stop/seek/fade/pan** | Só `destroy()` para o stream AAudio; o player OpenSLES é destruído na próxima chamada de `play_opensles()` |
| **Parser WAV minimalista** | Não valida tamanho RIFF, não exige `fmt ` antes de `data`, aceita `fmt ` maior que 16 bytes (desde que `audio_format == 1`), ignora chunks extras e descarta byte ímpar de `data` |
| **Buffer manual inválido é ignorado** | `play()` exige 1–2 canais, `sample_rate > 0` e `samples.size() % channels == 0`; um buffer fora disso vira no-op silencioso |

---

## 12. Armadilhas e diagnóstico

### 12.1 `AAssetManager` do App (e o caminho do WAV)

`load_wav()` chama `Assets::instance(nullptr)`. Isso funciona porque o `App`
injetou o gerenciador antes de `on_ready()`:

```cpp
// app.hpp:192 — dentro de activate_surface()
Assets::instance(app_->activity).set_activity(app_->activity);
```

- Se você usar `Audio` **fora** de um `App`, faça isso manualmente:
  `Assets::instance(activity).set_activity(activity);`. Sem gerenciador,
  `load_wav()` retorna `false` sem log.
- O caminho é relativo à raiz de `assets/` (`AAssetManager_open`). O exemplo do
  `README.md` com `"assets/efeito.wav"` não corresponde ao código; use
  `"efeito.wav"`.
- `Assets::read()` devolve vetor vazio para arquivo inexistente; `load_wav()`
  transforma isso em `false` na checagem de 44 bytes.

### 12.2 Bloqueio na thread de render

`play()` no caminho AAudio chama `AAudioStream_write()` de forma bloqueante (até
1 s por chamada). `load_wav()` lê o arquivo inteiro e aloca um segundo vetor.
Chame `init()`, `load_wav()` e `play()` em `on_ready()` ou em handlers de
evento/toque — não em `on_draw()` nem em `on_tick()` a cada frame.

### 12.3 Volume: quando ele entra em vigor

`set_volume()` clampa o valor em `volume_` e esse campo é lido por
`gain_for()` dentro de `play_aaudio()` / `play_opensles()`. Ou seja:
**ajuste antes de chamar `play()`**. O OpenSLES escala uma **cópia** de
`samples`, mantendo o buffer original intacto; o AAudio escala a saída já
resampleada. Como o fallback OpenSLES é inalcançável (§6.2), na prática o
controle de volume depende do caminho AAudio.

### 12.4 Falha silenciosa de `init()` e log enganoso

- Só há log quando AAudio falha e o fallback é tentado (tag `nativeapp-audio`).
  Falha de OpenSLES não gera log; verifique o **retorno** de `init()`.
- `using_aaudio() == false` é ambíguo: pode ser fallback, build `--api <= 25` ou
  `init()` nem ter sido chamado.
- No `device-demo`, `init()` e `using_aaudio()` são avaliados dentro da mesma
  chamada de `LOGI`; a ordem de avaliação dos argumentos não é especificada,
  então o campo "AAudio" do log pode mostrar `0` mesmo com inicialização
  correta. Separe em variáveis (como no §9).

### 12.5 `play()` antes de `init()` e depois de `destroy()`

Com os ponteiros nulos, `play()` retorna sem fazer nada (não há exceção nem
log). Após `destroy()`, chame `init()` novamente. Troca de superfície não
destrói o áudio automaticamente — o `App` não chama `Audio::destroy()` nesses
eventos.

### 12.6 `minSdk` não escolhe o backend

O `#if __ANDROID_API__` é controlado por `--api` na compilação. Mudar apenas
`--min-sdk` no manifesto não habilita nem desabilita AAudio. Para desabilitar,
recompile com `--api 21` (o builder então não passa `-laaudio`).

### 12.7 Diagnóstico rápido

| Sintoma | Causa provável no código |
|---|---|
| `load_wav()` sempre `false` | `AAssetManager` não injetado, caminho com prefixo `assets/`, ou WAV não-PCM/8-bit/24-bit/extensible |
| `init()` `true`, mas nenhum som | Fallback OpenSLES: `play()` não chama `play_opensles()` (§6.2) |
| Som some no device API 21–25 com build `--api >= 26` | Dependência `libaaudio.so` e/ou chamadas AAudio compiladas; não há `dlopen` opcional |
| Volume não muda | Fallback OpenSLES (caminho inalcançável), `set_volume()` chamado **depois** de `play()`, ou valor NaN/∞ clampado para `1.0f` |
| "Travada" a cada frame | `AAudioStream_write()` bloqueante chamado na thread de render |

---

## 13. Resumo rápido

```text
App (on_ready)
  └─ Audio::instance().init()                  → true/false (silencioso no fallback)
       ├─ --api >= 26: AAudio 44100/2/PCM_I16  → using_aaudio() == true
       └─ senão/falha: OpenSLES engine+mix     → using_aaudio() == false

Carregar:  load_wav("efeito.wav", buf)         → PCM16, 1–2 canais, RIFF/WAVE
           load("musica.ogg", buf)             → via codecs.hpp (WAV/OGG/MP3)
Tocar:     play(buf, v)                        → AAudio: resample 44100/2ch + ganho
                                                 OpenSLES: ganho, mas caminho inalcançável
Ajustar:   set_volume(v) → volume()            → ganho global da próxima play()
Limpar:    destroy() (ou destrutor)            → idempotente; fecha stream/objetos
```

Checklist antes de culpar o dispositivo:

1. `--api >= 26` se quiser AAudio; `-laaudio` é automático no builder.
2. WAV em `<projeto>/assets/`, PCM 16-bit, qualquer taxa > 0 (AAudio resampleia
   para 44100), 1 ou 2 canais.
3. Caminho passado sem o prefixo `assets/`.
4. `init()` chamado e retorno verificado; `play()` fora da thread de desenho.
5. `set_volume()` **antes** de `play()`; valores NaN/∞ viram `1.0f`.
6. Lembre-se: **o fallback OpenSLES não reproduz** nesta versão — limitação real
   do código, não configuração faltando.
