# Teclado virtual (`nativeapp::TextInput`)

> Guia completo do helper de teclado virtual do apkfast.
>
> Fontes consultadas para este documento: `include/nativeapp/text_input.hpp` (API),
> `include/nativeapp/app.hpp` (inicialização e repasse de eventos) e
> `examples/text-input-hello/jni/main.cpp` (exemplo funcional).
> Tudo o que está aqui existe no código atual — não há APIs "planejadas" nesta página.

## Índice

1. [Visão geral: o problema e a solução](#1-visão-geral-o-problema-e-a-solução)
2. [Contexto histórico: por que as abordagens óbvias falharam](#2-contexto-histórico-por-que-as-abordagens-óbvias-falharam)
3. [Compatibilidade (API 21+, 35+ e fallback)](#3-compatibilidade-api-21-35-e-fallback)
4. [Inicialização: quem chama `instance(env, activity)`](#4-inicialização-quem-chama-instanceenv-activity)
5. [Tabela completa de métodos públicos](#5-tabela-completa-de-métodos-públicos)
6. [Callback, eventos de texto e ausência de enum](#6-callback-eventos-de-texto-e-ausência-de-enum)
7. [Como o Unicode do IME é obtido (internals)](#7-como-o-unicode-do-ime-é-obtido-internals)
8. [Exemplo completo: App + GLES, toque abre o teclado](#8-exemplo-completo-app--gles-toque-abre-o-teclado)
9. [Integração com SDL2 / SDL3](#9-integração-com-sdl2--sdl3)
10. [Armadilhas e limitações](#10-armadilhas-e-limitações)
11. [Como validar no dispositivo](#11-como-validar-no-dispositivo)
12. [Resumo rápido](#12-resumo-rápido)

---

## 1. Visão geral: o problema e a solução

Uma `NativeActivity` desenha em uma superfície nativa (EGL/Vulkan) e **não tem um
`EditText`**. Não existe, no mundo C/C++, um campo de texto nativo para onde o IME
(Input Method Editor — o teclado virtual do Android) possa enviar caracteres.
Ainda assim, o Android continua emitindo **eventos de tecla** (`AINPUT_EVENT_TYPE_KEY`)
para a Activity quando o teclado está aberto — e é exatamente isso que o apkfast usa.

A solução do `nativeapp::TextInput` tem quatro peças:

1. **`InputMethodManager.toggleSoftInput(SHOW_FORCED, 0)`** via JNI puro — isto
   abre o teclado **sem precisar de nenhuma `View`**. (`show()` usa os flags `2, 0`;
   `hide()` chama `toggleSoftInput(0, 1)` e marca o estado interno como fechado.)
2. **O IME envia key events** para a Activity; o `App` os recebe em `on_input()`.
3. **`TextInput::unicode_from_event()`** extrai o caractere real (acentos,
   símbolos, IMEs internacionais), com caminho nativo em API 35+ e reconstrução
   de `KeyEvent` via JNI em API 21–34.
4. **`TextInput::on_key()`** interpreta o resultado: caracteres imprimíveis são
   anexados ao texto, Backspace apaga um codepoint UTF-8, ENTER finaliza, BACK
   esconde o teclado. Um callback opcional avisa a aplicação.

Fluxo completo quando se usa `nativeapp::App`:

```text
[toque na tela]  ->  App::on_touch()  ->  TextInput::show()
                                             |
                                             v
                       InputMethodManager.toggleSoftInput(2, 0)

[teclado virtual] ->  AINPUT_EVENT_TYPE_KEY
                       ->  App::on_input()
                           ->  TextInput::unicode_from_event(event)
                           ->  TextInput::on_key(code, down, unicode)
                               ->  callback(texto, done)
```

O helper **não é um editor de texto completo**: não há seleção, cursor,
composição de texto, sugestões, `EditText` nem renderização de glyphs. Ele mantém
uma `std::string` com o que foi digitado e avisa você por callback. Desenhar o
texto (ou reagir a ele) é responsabilidade da aplicação — o exemplo abaixo muda a
cor da cena.

> **Sem Java do app, mas com JNI:** o helper chama classes do *framework* Android
> (`android.view.inputmethod.InputMethodManager`, `android.content.Context`,
> `android.view.KeyEvent`) via JNI. Não existe nenhuma classe Java escrita pelo
> usuário, nenhum `EditText` e nenhum `classes.dex` extra para o teclado.

---

## 2. Contexto histórico: por que as abordagens óbvias falharam

O histórico do repositório explica o desenho atual:

| Fase | Abordagem | Motivo da mudança |
|---|---|---|
| `bac328d` — primeira versão | `EditText` invisível (1×1 px, fundo transparente) criado via JNI, anexado com `Activity.addContentView()`, `requestFocus()`, `showSoftInput(view, 0)`; texto lido por `update()` (polling) e `is_done()` | Dependia de hierarquia de `View`, foco e **window token** (`getWindowToken()` + `hideSoftInputFromWindow()`). O commit seguinte descreve a reescrita como "no custom Java, no class loader issues, no window tokens". |
| `39aebdf` — reescrita | `toggleSoftInput()` sem `View`; texto via key events do IME; `App` repassa os eventos; Backspace UTF-8 aware, ENTER encerra, BACK esconde | Removeu `EditText`, `addContentView()`, tokens de janela e a necessidade de polling. É o modelo usado até hoje. |
| `aa86b50` — compatibilidade | `unicode_from_event()` com `AInputEvent_toJava()` em API 35+ e reconstrução de `KeyEvent` via JNI em API 21–34 | `AInputEvent_toJava()` só existe em API 35+; sem o fallback, acentos e IMEs internacionais falhavam em dispositivos mais antigos. |

Sobre a **classe Java customizada com `ClassLoader`**: ela foi o caminho
considerado e **descartado** — não existe no `text_input.hpp` atual. O registro da
reescrita (`39aebdf`) diz explicitamente *"no custom Java, no class loader issues,
no window tokens"*. Introduzir uma classe própria exigiria incluí-la/dexá-la no APK
e localizá-la via `ClassLoader` (um padrão visível apenas nas classes Java dos
exemplos SDL, para `ReLinker`, sem relação com o teclado), o que quebra a proposta
do apkfast de ser um pipeline sem Gradle e um helper de teclado *header-only*.

**Conclusão honesta:** as abordagens com `EditText` e com Java customizado não
são "erradas" em todo app Android — elas apenas exigem recursos (View, foco,
token, dex, carregador de classes) que um app nativo com `NativeActivity` não
quer gerenciar. O `TextInput` evita todos eles.

---

## 3. Compatibilidade (API 21+, 35+ e fallback)

A compatibilidade documentada pelo projeto é **API 21+** (`CHANGELOG.md` descreve
`TextInput` como "teclado virtual sem EditText nem Java (API 21+)"). O mecanismo
de abertura do teclado (`toggleSoftInput`) é uma API antiga — o comentário no topo
do header chega a dizer "API 1+" —, mas a garantia de Unicode correto do apkfast
começa na API 21.

| Situação | Caminho usado | Onde |
|---|---|---|
| **API 35+** (compilado com `__ANDROID_API__ >= 35` e sem `APKFAST_TEST_LEGACY_IME`) | `AInputEvent_toJava(env, event)` → `KeyEvent.getUnicodeChar()` | `TextInput::unicode_from_event()` |
| **API 21–34**, ou build 35+ com a macro `APKFAST_TEST_LEGACY_IME` definida | Reconstrói `android.view.KeyEvent` via JNI com os 8 campos nativos e chama `getUnicodeChar(meta)` | `TextInput::unicode_from_event()` |
| **Qualquer API**, quando o Unicode veio `0` (tecla de controle, IME sem Unicode, `env_ == nullptr`, sem evento) | Mapeamento ASCII embutido de keycode → caractere | `TextInput::on_key()` |

Detalhes importantes:

- A macro `APKFAST_TEST_LEGACY_IME` existe no header para **forçar o fallback
  legado mesmo em builds 35+** (foi usada na validação dos dois caminhos). Não é
  uma API pública de uso normal.
- Se o caminho 35+ retornar `0`, o código **cai para o fallback JNI**; exceções
  pendentes são limpas (`ExceptionCheck()` / `ExceptionClear()`).
- No fallback JNI, exceção pendente é reportada com `ExceptionDescribe()` e
  limpa, e a função retorna `0`.
- O mapeamento ASCII cobre letras `a–z` (keycodes 29–54), dígitos `0–9`
  (7–16), espaço, `,`, `.`, `-`, `;`, `/`, `` ` ``, `'`, `[`, `]`, `\`. Não
  cobre maiúsculas nem acentos — para isso, é preciso o Unicode do IME.
- Caracteres Unicode `10` (`\n`) e `13` (`\r`) são **ignorados** por `on_key()`:
  ENTER é tratado como confirmação, não como quebra de linha.

---

## 4. Inicialização: quem chama `instance(env, activity)`

Quando você usa `nativeapp::App`, **não precisa inicializar nada**. Em
`App::activate_surface()`, na primeira janela válida e **antes de `on_ready()`**, o
framework faz:

```cpp
// include/nativeapp/app.hpp — activate_surface()
if (!initialized_) {
    initialized_ = true;
    Assets::instance(app_->activity).set_activity(app_->activity);

    // Injeta o JNIEnv/Activity da UI thread no TextInput.
    JNIEnv* ui_env = nullptr;
    if (app_->activity->vm) {
        app_->activity->vm->GetEnv(reinterpret_cast<void**>(&ui_env),
                                   JNI_VERSION_1_6);
    }
    if (ui_env) {
        TextInput::instance(ui_env, app_->activity->clazz);
    }
    on_ready();
    state_ = Lifecycle::Ready;
}
```

O `JNIEnv*` é obtido na thread da UI via `vm->GetEnv(..., JNI_VERSION_1_6)` e a
Activity é `app_->activity->clazz`. Depois disso, o singleton guarda `env_` e
`activity_` internamente.

É por isso que o exemplo (`examples/text-input-hello/jni/main.cpp`) pode chamar:

```cpp
auto& input = TextInput::instance(nullptr, nullptr);
```

`instance()` cria o singleton apenas na primeira chamada e **só substitui**
`env_`/`activity_` quando ambos os argumentos são não nulos e a Activity mudou:

```cpp
static TextInput* singleton = nullptr;
if (!singleton) singleton = new TextInput(env, activity);
if (env && activity && singleton->activity_ != activity) {
    singleton->activity_ = activity;
    singleton->env_ = env;
}
```

Consequências práticas:

- Dentro de um `App`, `instance(nullptr, nullptr)` devolve o mesmo objeto já
  inicializado — inclusive em `on_ready()`, `on_touch()` e `on_key()`.
- O próprio `App::on_input()` usa `instance(nullptr, nullptr)` para consultar
  `is_shown()`, calcular `unicode_from_event()` e chamar `on_key()`.
- **Sem `App`** (NativeActivity puro, raylib, SDL), a sua aplicação deve fazer a
  primeira chamada com `JNIEnv*` e `activity` válidos, na thread correta, antes de
  `show()`/`hide()`/`unicode_from_event()`.

---

## 5. Tabela completa de métodos públicos

Classe: `nativeapp::TextInput` (`include/nativeapp/text_input.hpp`). Todos os
métodos abaixo são `public`; as assinaturas são exatamente as do header.

| Método | Assinatura | O que faz |
|---|---|---|
| `instance` | `static TextInput& instance(JNIEnv* env, jobject activity)` | Singleton. Cria na primeira chamada; atualiza `env_`/`activity_` se ambos forem não nulos e a Activity mudou. |
| `show` | `void show()` | Abre o teclado com `InputMethodManager.toggleSoftInput(2, 0)` (sem `View`). Sai silenciosamente se não houver `env_`/`activity_`; marca `shown_ = true` quando encontra o método `toggleSoftInput`. |
| `hide` | `void hide()` | Fecha com `toggleSoftInput(0, 1)` e marca `shown_ = false` (mesmo que não consiga obter o serviço). |
| `text` | `std::string text() const` | Devolve uma **cópia** da string interna. |
| `set_text` | `void set_text(const std::string& text)` | Substitui o texto interno. **Não** dispara callback. |
| `clear` | `void clear()` | Esvazia o texto. **Não** dispara callback. |
| `is_shown` | `bool is_shown() const` | Estado interno `shown_` (controlado por `show()`/`hide()`/`on_key()`/`destroy()`), não uma consulta ao IME. |
| `set_callback` | `void set_callback(Callback callback)` | Registra o callback de texto; `Callback` é `std::function<void(const std::string&, bool)>`. |
| `on_key` | `bool on_key(int32_t keycode, bool down, int32_t unicode = 0)` | Interpreta a tecla; devolve `true` se consumiu. Ignora eventos `up` e eventos quando o teclado não está aberto. |
| `unicode_from_event` | `int32_t unicode_from_event(AInputEvent* event) const` | Unicode real do evento: API 35+ via `AInputEvent_toJava`, fallback JNI em 21–34. Devolve `0` para evento nulo, evento que não seja de tecla ou sem `env_`. |
| `env` | `JNIEnv* env() const` | Devolve o `JNIEnv*` guardado (pode ser `nullptr`). |
| `destroy` | `void destroy()` | Zera `shown_` e limpa `text_`. **Não** chama `hide()` e não libera referências JNI (o helper não guarda global refs). |

Tipos públicos declarados no header:

```cpp
using Callback = std::function<void(const std::string&, bool /*done*/)>;
enum class KeyAction { None, Character, Backspace, Enter };
```

`KeyAction` está declarado, mas **não é usado por nenhum método nem pelo
callback** — ele não deve ser confundido com um "evento de texto" da API atual.

---

## 6. Callback, eventos de texto e ausência de enum

### Assinatura

```cpp
input.set_callback([](const std::string& text, bool done) {
    // text  = estado ATUAL do texto (depois da alteração)
    // done  = true somente quando ENTER confirmou
});
```

O callback é **síncrono**, chamado de dentro de `on_key()` (ou seja, durante o
processamento do key event pelo `App`). O `std::string` recebido já contém a
modificação: caractere acrescentado ou codepoint removido.

### Quando dispara

| Situação em `on_key()` | Efeito no texto | Callback |
|---|---|---|
| Caractere imprimível (`unicode > 0`, exceto 10/13) | `append_utf8(unicode)` | `(texto, false)` |
| Unicode indisponível, fallback ASCII | `text_ += ch` | `(texto, false)` |
| `BACKSPACE` (67) ou `FORWARD_DEL` (112), com texto não vazio | Remove **um codepoint UTF-8** | `(texto, false)` |
| `BACKSPACE`/`FORWARD_DEL` com texto vazio | Nada | Não dispara |
| `ENTER` (66), `DPAD_CENTER` (23) ou `NUMPAD_ENTER` (160) | Nada | `(texto, true)` e depois `hide()` |
| `BACK` (4) | Nada | Não dispara; apenas `hide()` |
| `set_text()` / `clear()` | Substitui/limpa | Não dispara |
| Evento `up`, teclado fechado ou keycode desconhecido | Nada | Não dispara |

`done == false` significa "o texto mudou"; `done == true` significa "ENTER
finalizou". É essa diferença que substitui qualquer enum de evento.

### Backspace UTF-8 aware

O código não apaga um *byte*: ele caminha para trás enquanto o byte estiver no
intervalo de continuação UTF-8 (`(byte & 0xC0) == 0x80`) e remove a sequência
inteira. Apagar `é`, `♥` ou um emoji não deixa bytes órfãos na string.

### ENTER finaliza e esconde

Para ENTER/DPAD_CENTER/NUMPAD_ENTER a ordem é: callback com `done = true`,
`hide()`, `return true` (evento consumido). O texto permanece em `text()` — o
`hide()` não limpa nada. Se quiser limpar, chame `clear()` explicitamente.

### Não existe enum de eventos na API atual

Não procure por `TextInputEvent`, `Event` ou algo parecido: a API atual usa o
`bool done`. O `App` também não devolve um "retorno do `on_key`" para você: o
callback virtual é `void on_key(int32_t, bool)`. O `1`/`0` visto em `App::on_input()`
é o retorno interno para o framework (teclas consumidas pelo `TextInput` e
eventos de motion reconhecidos devolvem `1`).

Métodos antigos de polling (`update()`, `is_done()`, `clear_done()`, `show(hint)`,
`set_hint()`, `set_multiline()`, `available()`) **não existem** mais.

---

## 7. Como o Unicode do IME é obtido (internals)

`App::on_input()` não usa `AKeyEvent_getUnicodeChar()` diretamente. Ele delega:

```cpp
// include/nativeapp/app.hpp — on_input(), trecho simplificado
if (TextInput::instance(nullptr, nullptr).is_shown()) {
    const int32_t unicode =
        TextInput::instance(nullptr, nullptr).unicode_from_event(event);
    if (TextInput::instance(nullptr, nullptr).on_key(code, down, unicode)) {
        return 1;  // consumido: seu on_key() NÃO é chamado
    }
}
on_key(code, down);
return 0;
```

> O comentário de uso no topo de `text_input.hpp` mostra
> `AKeyEvent_getUnicodeChar(event)`. Ele é genérico; em um app baseado em
> `nativeapp::App`, use `unicode_from_event(event)` — é o que o framework já faz
> e é o único caminho que cobre API 21–34.

### Caminho 35+

```cpp
jobject java_event = AInputEvent_toJava(env, event);
// ... getUnicodeChar() sem argumentos no objeto convertido ...
```

Se houver resultado diferente de `0`, ele é devolvido. Se não houver, a função
limpa exceções e continua para o fallback.

### Caminho 21–34 (fallback JNI)

O NDK fornece todos os campos nativos do evento; o helper reconstrói um
`android.view.KeyEvent` com o construtor de assinatura `(JJIIIIII)V` e chama
`getUnicodeChar(meta)`:

| Campo nativo | Origem | Argumento do construtor |
|---|---|---|
| down time | `AKeyEvent_getDownTime(event)` | `jlong` |
| event time | `AKeyEvent_getEventTime(event)` | `jlong` |
| action | `AKeyEvent_getAction(event)` | `jint` |
| key code | `AKeyEvent_getKeyCode(event)` | `jint` |
| repeat count | `AKeyEvent_getRepeatCount(event)` | `jint` |
| meta state | `AKeyEvent_getMetaState(event)` | `jint` (também passado a `getUnicodeChar(meta)`) |
| device id | `AInputEvent_getDeviceId(event)` | `jint` |
| scan code | `AKeyEvent_getScanCode(event)` | `jint` |

Passar o `meta state` para `getUnicodeChar(meta)` é o que permite resolver
acentos e combinações de teclados internacionais. O header registra que esses
dados nativos estão disponíveis desde as primeiras versões do Android; a
compatibilidade garantida pelo projeto, porém, é API 21+.

### Último recurso: ASCII

Se `unicode_from_event()` devolver `0` — ou se você chamar `on_key()` com o
valor padrão `unicode = 0` —, `on_key()` tenta `ascii_from_keycode(keycode)`.
Isso mantém digitação básica funcionando mesmo sem IME cooperativo, mas perde
acentos e símbolos não mapeados.

---

## 8. Exemplo completo: App + GLES, toque abre o teclado

Este é o exemplo `examples/text-input-hello/jni/main.cpp`, reproduzido por
inteiro. Ele abre/fecha o teclado a cada toque, escreve no logcat, muda a cor de
fundo conforme o tamanho do texto, desenha um quadrado com GLES2 e trata BACK:

```cpp
// Teclado virtual sem EditText — NativeActivity + nativeapp::TextInput.
// O texto digitado aparece no logcat (tag "text-input") e muda a cor da cena.
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#define LOG_TAG "text-input"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class TextApp : public App {
public:
    explicit TextApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        set_scene(&scene_);
        scene_.set_clear_color(0.05f, 0.05f, 0.10f);

        auto& input = TextInput::instance(nullptr, nullptr);
        input.set_callback([this](const std::string& text, bool done) {
            typed_ = text;
            LOGI("texto='%s' done=%d", text.c_str(), done);
            // Muda a cor conforme o tamanho do texto (feedback visual).
            const float t = static_cast<float>(text.size()) / 20.0f;
            scene_.set_clear_color(0.05f + t * 0.5f, 0.05f, 0.10f + t * 0.3f);
        });
        LOGI("ready");
    }

    void on_surface_created() override { scene_.init(window()); }
    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_draw() override {
        // Cena gerencia begin_frame/present; apenas desenhamos um quadrado.
        Material material;
        Mat4 model = mat4_translate(width() / 2.0f, height() / 2.0f, 0.0f);
        model = mat4_multiply(model, mat4_scale(200.0f, 200.0f, 1.0f));
        scene_.draw(tile_, material, model.data());
    }

    void on_touch(TouchAction action, const std::vector<TouchPoint>&) override {
        if (action != TouchAction::Down) return;
        auto& input = TextInput::instance(nullptr, nullptr);
        if (!input.is_shown()) {
            input.show();
            LOGI("show() -> is_shown=%d", input.is_shown());
        } else {
            input.hide();
        }
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == 4 /* BACK */) {
            auto& input = TextInput::instance(nullptr, nullptr);
            if (input.is_shown()) input.hide();
            else request_exit();
        }
    }

    GLES2Scene scene_;
    MeshData tile_ = MeshData::quad();
    std::string typed_;
};

APP_MAIN(TextApp)
```

Pontos a notar:

- `on_ready()` é o lugar natural para `set_callback()`: o `App` já injetou
  `JNIEnv`/Activity no singleton antes de chamá-lo.
- `on_touch()` alterna `show()`/`hide()` usando `is_shown()`.
- `on_key()` só recebe BACK quando o `TextInput` **não** consumiu o evento. Com o
  teclado aberto, `TextInput::on_key()` consome BACK e chama `hide()`; o `else`
  do exemplo (`request_exit()`) acontece somente com o teclado fechado.
- `typed_` guarda a última string recebida no callback; não há API para
  "desenhar texto" no `TextInput` — o feedback visual aqui é a cor da cena.
- `#include <nativeapp/nativeapp.hpp>` já traz `app.hpp`, que inclui
  `text_input.hpp`; não é necessário incluir o header do teclado à parte.

---

## 9. Integração com SDL2 / SDL3

Se você usa `nativeapp::App`, **pare aqui**: o `App` já chama
`TextInput::instance(ui_env, app_->activity->clazz)` em `activate_surface()` e já
repassa os key events. Não faça uma segunda inicialização.

Em um app SDL, a Activity e o `JNIEnv*` vêm das funções do próprio SDL. As
chamadas existentes nos headers vendidos são:

| Engine | `JNIEnv*` | Activity (`jobject`) |
|---|---|---|
| SDL2 | `SDL_AndroidGetJNIEnv()` | `SDL_AndroidGetActivity()` |
| SDL3 | `SDL_GetAndroidJNIEnv()` | `SDL_GetAndroidActivity()` |

Exemplo SDL3 (o padrão já usado por `examples/sdl3-permissions` para
`JniPermissions`):

```cpp
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_main.h>
#include <android_native_app_glue.h>
#include <nativeapp/text_input.hpp>

JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());

auto& input = nativeapp::TextInput::instance(env, activity);
input.set_callback([](const std::string& text, bool done) {
    SDL_Log("texto='%s' done=%d", text.c_str(), done);
});
input.show();   // por exemplo, ao abrir um diálogo de texto no seu app SDL
```

Exemplo SDL2 (mesmo fluxo, nomes antigos):

```cpp
#include <SDL.h>
#include <SDL_system.h>
#include <nativeapp/text_input.hpp>

JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());

auto& input = nativeapp::TextInput::instance(env, activity);
input.show();
```

SDL3 também fornece aliases de compatibilidade (`SDL_AndroidGetActivity` e
`SDL_AndroidGetJNIEnv`) em `SDL3/SDL_oldnames.h`, mas os nomes nativos da SDL3
são os da primeira tabela.

**Limite importante:** fora do `nativeapp::App`, ninguém chama
`unicode_from_event()`/`on_key()` por você. O `TextInput` sozinho abre e fecha o
teclado e guarda o texto, mas o seu loop SDL precisa repassar as teclas
(chamando `on_key(code, down, unicode)` com keycode/Unicode adequados) se quiser
que a digitação seja processada pelo helper. Além disso, SDL tem seu próprio
sistema de texto (`SDL_StartTextInput()`); escolha um dos dois caminhos para não
duplicar a entrada.

---

## 10. Armadilhas e limitações

| Armadilha | O que acontece | Como lidar |
|---|---|---|
| **Activity sem foco** | `show()` apenas verifica `env_`/`activity_`; ele não garante que a janela esteja focada. Sem foco, o IME pode não aparecer (ou aparecer e não entregar teclas). | Chame `show()` com a janela ativa/focada. Com `App`, `focused()` reflete `APP_CMD_GAINED_FOCUS`/`LOST_FOCUS`; confirme com `dumpsys input_method`. |
| **Unicode depende do layout/IME** | `getUnicodeChar(meta)` devolve o caractere que o IME/layout atual produz. O keycode é só a tecla física; acentos/CJK dependem do caminho Unicode. | Use `unicode_from_event()` (feito pelo `App`); teste com outro layout/IME antes de culpar o helper. |
| **Fallback ASCII é só fallback** | Sem Unicode, só `a–z`, `0–9` e alguns sinais são mapeados; maiúsculas e acentos não. | Não confie no fallback para texto internacional; garanta `env_` válido e o caminho JNI. |
| **Não chame `hide()` dentro do callback de texto** | O callback roda **dentro** de `on_key()`, que ainda está processando o evento. `hide()` altera `shown_` no meio do caminho e, em ENTER, o próprio `on_key()` já chama `hide()` logo após o callback — isso pode duplicar a alternância do IME e deixar eventos seguintes ignorados. | Reaja ao `done == true` fora do callback (por exemplo, no seu loop/`on_tick`), ou deixe o `hide()` automático do ENTER agir. |
| **`is_shown()` é estado interno** | Não consulta o InputMethodManager de verdade. | Use-o para alternar `show()`/`hide()` no seu app, não como diagnóstico do IME. |
| **`destroy()` não esconde o teclado** | Ele só faz `shown_ = false` e `text_.clear()`. | Chame `hide()` antes, se precisar fechar o teclado. |
| **`set_text()`/`clear()` não avisam** | Nenhum callback é disparado. | Atualize sua UI diretamente nesses casos. |
| **`screencap` do adb pode enganar** | Já registrado no `AUDIT.md`: no dispositivo Samsung usado nos testes, `screencap` capturava apenas o clear color das surfaces EGL, **não o conteúdo renderizado**, gerando falsa impressão de "não desenhou". | Valide com `glReadPixels`, logs e `dumpsys input_method` — não apenas com screenshot. |
| **Texto "multilinha" não existe mais** | Unicode `10`/`13` é descartado e ENTER sempre finaliza. | Trate ENTER como confirmação; a versão antiga com `set_multiline()` não faz parte da API atual. |
| **Chamar `instance()` com argumentos nulos antes de qualquer inicialização** | Se ainda não houve `instance(env, activity)` nem `App`, `env_`/`activity_` ficam nulos: `show()`/`hide()` não fazem nada e `unicode_from_event()` devolve `0`. | Sem `App`, faça a primeira chamada com `JNIEnv*` e Activity válidos. |

---

## 11. Como validar no dispositivo

```bash
# O teclado realmente está visível?
adb shell dumpsys input_method | grep -i mInputShown
# esperado depois de show(): mInputShown=true

# Logs do exemplo e do helper
adb logcat -s text-input nativeapp-text
```

Sinais esperados com `examples/text-input-hello`:

- ao tocar: `show() -> is_shown=1`;
- ao digitar: `texto='...' done=0` (com acentos se o IME/layout fornecer Unicode);
- ao apertar BACKSPACE: texto encurta sem bytes UTF-8 quebrados;
- ao apertar ENTER: `texto='...' done=1`, teclado escondido, app continua vivo;
- ao apertar BACK com teclado aberto: teclado esconde; com ele fechado: app sai;
- geometria/renderização: `glReadPixels` (o `AUDIT.md` registrou
  `nonbg=40000` pixels, exatamente o quadrado 200×200 do exemplo).

Também é possível forçar o caminho legado em um build com NDK 35+ definindo
`APKFAST_TEST_LEGACY_IME` na compilação; o projeto já validou os dois caminhos.

---

## 12. Resumo rápido

```cpp
// Com nativeapp::App: nada a inicializar.
auto& input = nativeapp::TextInput::instance(nullptr, nullptr);

input.set_callback([](const std::string& text, bool done) {
    // done == false: texto mudou | done == true: ENTER finalizou
});

input.show();                 // toque no botão/cena
// ... App repassa os key events automaticamente ...
std::string valor = input.text();
input.hide();                 // quando quiser fechar
```

- Sem `EditText`, sem `addContentView()`, sem Java do app, sem token de janela.
- API 21+ com Unicode real: `AInputEvent_toJava` em 35+, `KeyEvent` via JNI em
  21–34, ASCII como último recurso.
- Backspace apaga um codepoint UTF-8; ENTER chama o callback com `done = true` e
  esconde o teclado; BACK esconde.
- Não existe enum de eventos na API atual — apenas o `bool done`.
