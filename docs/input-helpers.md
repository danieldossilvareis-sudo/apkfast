# Helper de mouse/gamepad (`nativeapp::InputHelpers` / `input_helpers.hpp`)

> **Guia completo do helper de mouse/gamepad do apkfast.**
>
> **Fontes consultadas:** `include/nativeapp/input_helpers.hpp` (API e
> implementação), `include/nativeapp/app.hpp` (integração em `on_input()` e
> `run()`), `include/nativeapp/nativeapp.hpp` (header agregador),
> `include/nativeapp/graphics_types.hpp`, `include/nativeapp/math.hpp`,
> `include/nativeapp/gl.hpp` e `examples/nativeapp-gl/jni/main.cpp` (estilo de
> app usado no exemplo).
>
> **Regra deste documento:** só está aqui o que existe no código. Quando o
> texto tira uma consequência do código (em vez de transcrever um comentário),
> isso aparece como **Observação**.

---

## Índice

1. [Visão geral](#1-visão-geral)
2. [Quem alimenta o helper: `App::on_input()`](#2-quem-alimenta-o-helper-appon_input)
3. [`on_event(AInputEvent*)`: tipos e sources](#3-on_eventainputevent-tipos-e-sources)
4. [Teclado: `is_down()`, `last_key()` e `AKEYCODE_*`](#4-teclado-is_down-last_key-e-akeycode_)
5. [Gamepad: conexão, sticks, triggers e botões](#5-gamepad-conexão-sticks-triggers-e-botões)
6. [Mouse: posição, botões e scroll](#6-mouse-posição-botões-e-scroll)
7. [`reset()` e `consume_scroll()`](#7-reset-e-consume_scroll)
8. [Tabela completa de métodos](#8-tabela-completa-de-métodos)
9. [Exemplo completo: quadrado que segue o stick](#9-exemplo-completo-quadrado-que-segue-o-stick)
10. [Armadilhas e diagnóstico](#10-armadilhas-e-diagnóstico)
11. [Resumo rápido](#11-resumo-rápido)

---

## 1. Visão geral

`InputHelpers` é um **singleton header-only** (`#pragma once`, toda a
implementação inline dentro da classe) que guarda um "estado atual" de
teclado, mouse e gamepad a partir de eventos Android `AInputEvent`.

Ele **não tem bomba de eventos própria**, não abre looper, não registra
callback e não precisa de `init()`: quem repassa os eventos para ele é o
`nativeapp::App`, automaticamente.

```cpp
static InputHelpers& instance() {
    static InputHelpers singleton;
    return singleton;
}
```

- Uma instância por processo, com construtor privado (`InputHelpers() = default`).
- Acesso sempre via `InputHelpers::instance()`.
- O helper está incluído de duas formas:
  - diretamente: `#include <nativeapp/input_helpers.hpp>`;
  - pelo agregador: `#include <nativeapp/nativeapp.hpp>` — que inclui
    `app.hpp`, `gl.hpp`, `vk.hpp`, `sensor.hpp`, etc.; `app.hpp` já inclui
    `input_helpers.hpp` (linha 24).

```text
  ALooper_pollOnce(0, ...)  (App::run)
          │
          ▼
  source->process(app_, source)
          │
          ▼
  App::on_input_thunk  →  App::on_input(event)
          │
          ├──► InputHelpers::instance().on_event(event)   ← SEMPRE, antes de tudo
          │
          ├──► teclas → App::keys_down_ / TextInput / on_key()  → return 0
          │
          └──► toques → on_touch()                               → return 1
```

**Observação:** o `on_event()` é chamado na **primeira linha** de `on_input()`,
antes de qualquer filtragem de tipo/source feita pelo `App`. Portanto, enquanto
o app usa `App::run()` com o `onInputEvent` padrão, não é necessário (e não
deve ser feito) re-passar eventos manualmente.

**Observação:** se você usar o helper **fora** de um `App` (por exemplo, em um
loop de eventos próprio), aí sim cabe chamar `on_event()` você mesmo — o helper
não inventa eventos sozinho.

---

## 2. Quem alimenta o helper: `App::on_input()`

Em `App::run()`:

```cpp
app_->userData = this;
app_->onAppCmd = &App::on_app_command_thunk;
app_->onInputEvent = &App::on_input_thunk;
```

E o thunk:

```cpp
static int32_t on_input_thunk(android_app* app, AInputEvent* event) {
    return static_cast<App*>(app->userData)->on_input(event);
}
```

O começo de `on_input()` é literalmente:

```cpp
int32_t on_input(AInputEvent* event) {
    InputHelpers::instance().on_event(event);
    const int32_t type = AInputEvent_getType(event);
    // ...
}
```

### 2.1 O que o `App` faz com cada tipo (contexto)

| Tipo de evento | Caminho no `App::on_input()` | Retorno do `App` |
|---|---|---|
| `AINPUT_EVENT_TYPE_KEY` | Atualiza `keys_down_`, repassa ao teclado virtual (`TextInput`) se ele estiver aberto, chama `on_key(code, down)` | `1` se o `TextInput` consumiu; senão `0` (não consome teclas do sistema) |
| `AINPUT_EVENT_TYPE_MOTION` com action `DOWN`/`POINTER_DOWN`/`UP`/`POINTER_UP`/`MOVE`/`CANCEL` | Monta `std::vector<TouchPoint>` e chama `on_touch(TouchAction, points)` | `1` |
| Qualquer outro motion (por exemplo **hover**) | `default: return 0;  // hover/out-of-frame: ignora` | `0` |
| Outro tipo | `return 0` | `0` |

**Observação:** o helper registra o evento **antes** dessas decisões. Ou
seja, um evento de hover não gera `on_touch()`, mas ainda passa pelo
`InputHelpers::on_event()`; se a source for mouse, ele atualiza
`mouse_x()`/`mouse_y()` e os botões.

---

## 3. `on_event(AInputEvent*)`: tipos e sources

Assinatura: `void on_event(AInputEvent* event)`.

### 3.1 Eventos de teclado (`AINPUT_EVENT_TYPE_KEY`)

```cpp
const int32_t type = AInputEvent_getType(event);
if (type == AINPUT_EVENT_TYPE_KEY) {
    const int32_t code = AKeyEvent_getKeyCode(event);
    const int32_t action = AKeyEvent_getAction(event);
    const bool down = action == AKEY_EVENT_ACTION_DOWN ||
                      action == AKEY_EVENT_ACTION_MULTIPLE;
    if (down) keys_.insert(code);
    else if (action == AKEY_EVENT_ACTION_UP) keys_.erase(code);
    last_key_ = code;
    return;
}
```

| Action | Efeito em `keys_` | Efeito em `last_key_` |
|---|---|---|
| `AKEY_EVENT_ACTION_DOWN` | `insert(code)` | `code` |
| `AKEY_EVENT_ACTION_MULTIPLE` | `insert(code)` (entra no mesmo `down`) | `code` |
| `AKEY_EVENT_ACTION_UP` | `erase(code)` | `code` |
| Qualquer outra action | nenhum | `code` |

**Observação:** `last_key_` é atualizado em **todo** evento de teclado,
inclusive no `UP`. Ele é "o último código de tecla visto", não "a última tecla
atualmente pressionada".

### 3.2 Eventos de motion (`AINPUT_EVENT_TYPE_MOTION`)

```cpp
if (type != AINPUT_EVENT_TYPE_MOTION) return;

const int32_t source = AInputEvent_getSource(event);
const bool is_mouse = (source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE;
const bool is_gamepad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                        (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;

const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
const int32_t buttons = AMotionEvent_getButtonState(event);
```

| Source (`AInputEvent_getSource`) | `is_mouse` | `is_gamepad` | Campos atualizados |
|---|---|---|---|
| `AINPUT_SOURCE_MOUSE` | `true` | `false` | `mouse_x_`, `mouse_y_`, `mouse_buttons_`; `scroll_x_`/`scroll_y_` se `action == AMOTION_EVENT_ACTION_SCROLL` |
| `AINPUT_SOURCE_GAMEPAD` | `false` | `true` | sticks, triggers, `gamepad_connected_ = true` |
| `AINPUT_SOURCE_JOYSTICK` | `false` | `true` | sticks, triggers, `gamepad_connected_ = true` |
| Combinação com `MOUSE` **e** `GAMEPAD`/`JOYSTICK` | `true` | `true` | **os dois blocos** rodam no mesmo evento |
| `AINPUT_SOURCE_TOUCHSCREEN` (ou qualquer outra source) | `false` | `false` | nenhum campo do helper é alterado |

**Observação:** `is_gamepad` usa **OU** entre `GAMEPAD` e `JOYSTICK` — um
joystick sem o bit de gamepad já habilita leitura de sticks/triggers.

**Observação:** a action é mascarada com `AMOTION_EVENT_ACTION_MASK`, então o
índice do ponteiro (usado pelo `App` para `POINTER_DOWN`/`POINTER_UP`) é
descartado pelo helper.

**Observação:** o helper sempre lê o **ponteiro 0** (`AMotionEvent_get*(event,
..., 0)`) para posição de mouse, sticks e triggers.

---

## 4. Teclado: `is_down()`, `last_key()` e `AKEYCODE_*`

```cpp
bool is_down(int32_t keycode) const { return keys_.count(keycode) > 0; }
int32_t last_key() const { return last_key_; }
```

- `is_down(int32_t)` consulta o `std::unordered_set<int32_t> keys_`.
- `last_key()` devolve o `int32_t last_key_`, inicializado em `0`.
- Os códigos são os `AKEYCODE_*` de `<android/keycodes.h>` — incluído no
  próprio `input_helpers.hpp`.

Exemplos de uso com constantes reais da NDK:

```cpp
auto& in = nativeapp::InputHelpers::instance();

if (in.is_down(AKEYCODE_DPAD_LEFT))  { /* ... */ }
if (in.is_down(AKEYCODE_SPACE))      { /* ... */ }
if (in.is_down(AKEYCODE_BACK))       { /* ... */ }
if (in.is_down(AKEYCODE_BUTTON_A))   { /* ... */ }  // também é keycode!
if (in.is_down(AKEYCODE_BUTTON_START)) { /* ... */ }

int32_t codigo = in.last_key();  // 0 até o primeiro evento de teclado
```

> ### Sobre `KeyAction`
>
> **Não existe `KeyAction` neste helper.** Não há `enum KeyAction`, nem
> métodos de borda (`just_pressed`, `was_released`, etc.) em
> `input_helpers.hpp`. O que existe é apenas estado contínuo (`is_down`) e o
> último código visto (`last_key`). Se você precisa de "apertou agora", compare
> o estado entre frames no seu próprio código.

**Observação:** o `App` mantém um conjunto de teclas **separado** (`keys_down_`,
consultável por `App::is_key_down(int32_t)`), alimentado pelo mesmo evento.
`InputHelpers::is_down()` e `App::is_key_down()` leem conjuntos diferentes,
mas, no fluxo padrão, refletem o mesmo `AKeyEvent`.

---

## 5. Gamepad: conexão, sticks, triggers e botões

### 5.1 Estado analógico

```cpp
struct Stick { float x = 0, y = 0; };
```

```cpp
if (is_gamepad) {
    // Analog sticks (AXIS_X/Y = esquerdo, Z/RZ = direito)
    stick_left_  = {AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0),
                    AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0)};
    stick_right_ = {AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0),
                    AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0)};
    triggers_ = {AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0),
                 AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0)};
    gamepad_connected_ = true;
}
```

| Método | Retorno | Eixo NDK usado |
|---|---|---|
| `left_stick()` | `Stick{x, y}` | `AMOTION_EVENT_AXIS_X`, `AMOTION_EVENT_AXIS_Y` |
| `right_stick()` | `Stick{x, y}` | `AMOTION_EVENT_AXIS_Z`, `AMOTION_EVENT_AXIS_RZ` |
| `left_trigger()` | `float` (`triggers_[0]`) | `AMOTION_EVENT_AXIS_LTRIGGER` |
| `right_trigger()` | `float` (`triggers_[1]`) | `AMOTION_EVENT_AXIS_RTRIGGER` |

**Observação:** os valores são os crus do eixo (`AMotionEvent_getAxisValue`):
o helper **não** aplica deadzone, normalização, inversão de Y nem clamp.

### 5.2 `gamepad_connected()`

```cpp
bool gamepad_connected() const { return gamepad_connected_; }
```

Ela vira `true` quando chega **qualquer** motion event com source
`GAMEPAD` ou `JOYSTICK`. O `on_event()` **nunca** a coloca como `false`: a
única forma de voltar a `false` é `reset()`. Veja a armadilha em
[§10.1](#101-gamepad_connected_-só-vira-true).

### 5.3 Botões: são eventos de teclado

Os botões não vêm do `button state` do motion event; eles usam `is_down()` com
os keycodes reais:

```cpp
bool button_a()     const { return is_down(AKEYCODE_BUTTON_A); }
bool button_b()     const { return is_down(AKEYCODE_BUTTON_B); }
bool button_x()     const { return is_down(AKEYCODE_BUTTON_X); }
bool button_y()     const { return is_down(AKEYCODE_BUTTON_Y); }
bool button_start() const { return is_down(AKEYCODE_BUTTON_START); }
```

| Método | Constante usada |
|---|---|
| `button_a()` | `AKEYCODE_BUTTON_A` |
| `button_b()` | `AKEYCODE_BUTTON_B` |
| `button_x()` | `AKEYCODE_BUTTON_X` |
| `button_y()` | `AKEYCODE_BUTTON_Y` |
| `button_start()` | `AKEYCODE_BUTTON_START` |

O helper expõe **apenas esses cinco** botões. Outros botões (LB/RB, L3/R3,
DPad, SELECT/BACK, ...) podem ser consultados diretamente com
`is_down(AKEYCODE_...)`.

---

## 6. Mouse: posição, botões e scroll

### 6.1 `enum MouseButton` (o que realmente existe)

```cpp
enum class MouseButton { Left, Right, Middle, Back, Forward };
```

São **cinco** valores, nessa ordem. Não há outros.

### 6.2 Posição e botões

```cpp
if (is_mouse) {
    mouse_x_ = AMotionEvent_getX(event, 0);
    mouse_y_ = AMotionEvent_getY(event, 0);
    mouse_buttons_ = buttons;
    if (action == AMOTION_EVENT_ACTION_SCROLL) {
        scroll_x_ = AMotionEvent_getXOffset(event);
        scroll_y_ = AMotionEvent_getYOffset(event);
    }
}
```

- `mouse_x()` / `mouse_y()`: coordenadas do ponteiro 0 do último motion event
  de mouse; ambas começam em `0`.
- `mouse_down(MouseButton)`: testa bits do `AMotionEvent_getButtonState(event)`
  guardado em `mouse_buttons_`.

```cpp
bool mouse_down(MouseButton button) const {
    const int32_t bit = button == MouseButton::Left ? 1 :
                        button == MouseButton::Right ? 2 :
                        button == MouseButton::Middle ? 4 :
                        button == MouseButton::Back ? 8 : 16;
    return (mouse_buttons_ & bit) != 0;
}
```

| `MouseButton` | Bit testado |
|---|---|
| `Left` | `1` |
| `Right` | `2` |
| `Middle` | `4` |
| `Back` | `8` |
| `Forward` | `16` (ramo final da cadeia de ternários) |

**Observação:** o estado dos botões é o **estado atual do evento**; o helper
não guarda bordas de pressão/release para o mouse (ao contrário do que
`is_down()` faz para teclas, ele mantém o bitmask inteiro).

### 6.3 Scroll

```cpp
float scroll_x() const { return scroll_x_; }
float scroll_y() const { return scroll_y_; }

/// Consome o delta de scroll acumulado (zera após a leitura).
void consume_scroll(float& x, float& y) {
    x = scroll_x_;
    y = scroll_y_;
    scroll_x_ = scroll_y_ = 0;
}
```

- `scroll_x_`/`scroll_y_` só são escritos em
  `action == AMOTION_EVENT_ACTION_SCROLL`, via `AMotionEvent_getXOffset()` /
  `AMotionEvent_getYOffset()`.
- `consume_scroll(x, y)` **copia** os dois valores para as referências e
  **zera** o acumulado.
- `reset()` também zera o scroll.

**Observação:** um novo `ACTION_SCROLL` **substitui** os valores anteriores
(atribuição, não soma). O que "fica acumulado" é o delta pendente entre
eventos de scroll e a leitura: motion events de mouse que não são scroll não
apagam esse valor.

```cpp
auto& in = nativeapp::InputHelpers::instance();

float sx = 0.0f, sy = 0.0f;
in.consume_scroll(sx, sy);  // sx/sy = últimos offsets; helper volta a 0
```

---

## 7. `reset()` e `consume_scroll()`

```cpp
void reset() {
    keys_.clear();
    mouse_buttons_ = 0;
    scroll_x_ = scroll_y_ = 0;
    last_key_ = 0;
    stick_left_ = stick_right_ = {};
    triggers_ = {};
    gamepad_connected_ = false;
}
```

| Campo | Após `reset()` |
|---|---|
| `keys_` | vazio (`clear()`) |
| `last_key_` | `0` |
| `mouse_buttons_` | `0` |
| `scroll_x_`, `scroll_y_` | `0`, `0` |
| `stick_left_`, `stick_right_` | `{}` → `{x=0, y=0}` |
| `triggers_` | `{}` → `{0, 0}` |
| `gamepad_connected_` | `false` |
| `mouse_x_`, `mouse_y_` | `0`, `0` |

**Observação:** `reset()` é público e **não é chamado automaticamente** pelo
`App` em nenhum ponto de `app.hpp`. Cabe ao seu código decidir quando chamá-lo
(por exemplo, ao trocar de cena ou ao voltar de `on_pause()`).

**Observação:** `reset()` também zera `mouse_x_`/`mouse_y_` — após o reset,
`mouse_x()`/`mouse_y()` devolvem `0` até um novo evento de mouse chegar.

---

## 8. Tabela completa de métodos

Todos os métodos públicos de `InputHelpers` (o construtor é privado):

| # | Método | Retorno | O que faz |
|---|---|---|---|
| 1 | `InputHelpers::instance()` | `InputHelpers&` | Singleton estático local; ponto de entrada |
| 2 | `on_event(AInputEvent* event)` | `void` | Atualiza o estado a partir de um evento key/motion |
| 3 | `is_down(int32_t keycode) const` | `bool` | `keys_.count(keycode) > 0` |
| 4 | `last_key() const` | `int32_t` | Último keycode visto (inclusive em `UP`); começa em `0` |
| 5 | `gamepad_connected() const` | `bool` | Já houve motion de `GAMEPAD`/`JOYSTICK` desde o último `reset()` |
| 6 | `left_stick() const` | `Stick` | Eixos `X`/`Y` |
| 7 | `right_stick() const` | `Stick` | Eixos `Z`/`RZ` |
| 8 | `left_trigger() const` | `float` | Eixo `LTRIGGER` |
| 9 | `right_trigger() const` | `float` | Eixo `RTRIGGER` |
| 10 | `button_a() const` | `bool` | `is_down(AKEYCODE_BUTTON_A)` |
| 11 | `button_b() const` | `bool` | `is_down(AKEYCODE_BUTTON_B)` |
| 12 | `button_x() const` | `bool` | `is_down(AKEYCODE_BUTTON_X)` |
| 13 | `button_y() const` | `bool` | `is_down(AKEYCODE_BUTTON_Y)` |
| 14 | `button_start() const` | `bool` | `is_down(AKEYCODE_BUTTON_START)` |
| 15 | `mouse_x() const` | `float` | X do ponteiro 0 do último motion de mouse |
| 16 | `mouse_y() const` | `float` | Y do ponteiro 0 do último motion de mouse |
| 17 | `mouse_down(MouseButton button) const` | `bool` | Testa o bit do botão no estado do evento |
| 18 | `scroll_x() const` | `float` | Offset X pendente (não consome) |
| 19 | `scroll_y() const` | `float` | Offset Y pendente (não consome) |
| 20 | `consume_scroll(float& x, float& y)` | `void` | Copia para `x`/`y` e zera `scroll_x_`/`scroll_y_` |
| 21 | `reset()` | `void` | Limpa o estado conforme a tabela da [§7](#7-reset-e-consume_scroll) |

Tipos públicos definidos no mesmo header:

| Tipo | Definição |
|---|---|
| `enum class MouseButton` | `{ Left, Right, Middle, Back, Forward }` |
| `struct Stick` | `{ float x = 0, y = 0; }` |

> **Não existe `KeyAction`** neste arquivo — ver o aviso na [§4](#4-teclado-is_down-last_key-e-akeycode_).

---

## 9. Exemplo completo: quadrado que segue o stick

Exemplo autossuficiente no estilo dos exemplos do projeto. Usa
`GLES2Scene::draw()` com `MeshData::quad()` e `mat4_translate`/`mat4_scale`;
`set_scene()` faz o `App` chamar `begin_frame()`/`present()` em volta de
`on_draw()`.

```cpp
#include <nativeapp/nativeapp.hpp>   // traz app.hpp + gl.hpp + input_helpers.hpp

#include <algorithm>

using namespace nativeapp;

class GamepadSquareApp : public App {
public:
    explicit GamepadSquareApp(android_app* app) : App(app) {
        set_scene(&scene_);  // App gerencia begin_frame()/present()
    }

private:
    void on_ready() override {
        quadrado_ = MeshData::quad(80.0f, 80.0f);
    }

    void on_surface_created() override {
        scene_.init(window());
        scene_.resize(width(), height());
        centralizar();
    }

    void on_surface_destroyed() override {
        scene_.destroy();
    }

    void on_surface_changed(int w, int h) override {
        scene_.resize(w, h);
    }

    void on_update(float dt) override {
        auto& in = InputHelpers::instance();

        // START recentraliza (botões são keycodes).
        if (in.button_start()) centralizar();

        if (!in.gamepad_connected()) return;

        const Stick esq = in.left_stick();
        const Stick dir = in.right_stick();

        // Deadzone manual: o helper NÃO filtra nada.
        const auto dead = [](float v) {
            return (v > 0.18f || v < -0.18f) ? v : 0.0f;
        };

        x_ += (dead(esq.x) + dead(dir.x) * 0.5f) * kVelocidade * dt;
        y_ += (dead(esq.y) + dead(dir.y) * 0.5f) * kVelocidade * dt;

        // Mantém o quadrado dentro da janela.
        const float meio = 40.0f;
        x_ = std::clamp(x_, meio, std::max(meio, static_cast<float>(width()) - meio));
        y_ = std::clamp(y_, meio, std::max(meio, static_cast<float>(height()) - meio));
    }

    void on_draw() override {
        auto& in = InputHelpers::instance();

        Material mat;
        mat.depth_test = false;
        // A: muda a cor; B: deixa o quadrado azul; gatilhos: escala.
        if (in.button_a())      { mat.color[0] = 0.2f; mat.color[1] = 1.0f; mat.color[2] = 0.3f; }
        else if (in.button_b()) { mat.color[0] = 0.3f; mat.color[1] = 0.5f; mat.color[2] = 1.0f; }
        else                    { mat.color[0] = 1.0f; mat.color[1] = 1.0f; mat.color[2] = 1.0f; }

        const float escala = 1.0f + in.left_trigger() * 0.5f
                                  - in.right_trigger() * 0.25f;
        const Mat4 modelo = mat4_multiply(
            mat4_translate(x_, y_, 0.0f),
            mat4_scale(escala, escala, 1.0f));

        scene_.draw(quadrado_, mat, modelo.data());
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    void centralizar() {
        x_ = static_cast<float>(width()) * 0.5f;
        y_ = static_cast<float>(height()) * 0.5f;
    }

    GLES2Scene scene_;
    MeshData quadrado_;
    float x_ = 0.0f, y_ = 0.0f;
    static constexpr float kVelocidade = 260.0f;  // pixels/s
};

APP_MAIN(GamepadSquareApp)
```

**Observação sobre o exemplo:** as coordenadas de `MeshData::quad()` são em
pixels porque `GLES2Scene::resize()` configura
`mat4_ortho2d(width, height)` (origem no canto superior esquerdo). O eixo Y do
stick é usado direto; se o seu jogo preferir "Y para cima", inverta o sinal no
seu código — o helper devolve o valor bruto do eixo.

**Observação:** `on_update(float dt)` recebe o `delta_time_` do `App`, que já
vem limitado a `0.25f` após um retorno do background.

---

## 10. Armadilhas e diagnóstico

### 10.1 `gamepad_connected()` só vira `true`

O `on_event()` faz `gamepad_connected_ = true` e nunca escreve `false`. Se o
controle é desligado, o helper continua reportando `true` até você chamar
`reset()`. Não existe callback de "desconectou" neste helper.

### 10.2 `reset()` limpa o estado — e não é automático

`reset()` zera teclas, posição e botões do mouse, scroll, `last_key`, sticks,
triggers e `gamepad_connected_` (ver [§7](#7-reset-e-consume_scroll)).
O `App` não chama `reset()` sozinho: faça isso explicitamente se precisar de
um estado limpo.

### 10.3 Scroll é pendente até `consume_scroll()`

`scroll_x()`/`scroll_y()` apenas leem; não zeram. Um novo `ACTION_SCROLL`
substitui os offsets anteriores. Se você usar `scroll_x()` direto sem
`consume_scroll()`, o mesmo delta aparece em vários frames. Use
`consume_scroll(sx, sy)` uma vez por frame (ou `reset()`).

### 10.4 Eventos de teclado do sistema NÃO são consumidos pelo `App`

Em `App::on_input()`, depois de `on_key()`, o código é explícito:

```cpp
// Não consome teclas do sistema (volume, HOME, multimídia).
return 0;
```

O retorno `1` só acontece quando o `TextInput` (teclado virtual) consumiu a
tecla. Portanto, `InputHelpers` **vê** o evento (foi repassado antes), mas
isso não impede o Android de tratar volume/HOME/multimídia. Não tente "fixar"
isso consumindo no helper: `on_event()` retorna `void` e não tem poder de
consumo.

### 10.5 Hover de mouse NÃO é tratado como toque (`app.hpp`)

No `App::on_input()`, o `switch` de actions de motion só aceita
`DOWN`, `POINTER_DOWN`, `UP`, `POINTER_UP`, `MOVE` e `CANCEL`. Qualquer outro
action cai em:

```cpp
default: return 0;  // hover/out-of-frame: ignora
```

Consequência: mover o mouse sobre a tela **não** chama `on_touch()` e não
alimenta `touches_down_`/`last_touches_`. Para posição do mouse use
`InputHelpers::mouse_x()`/`mouse_y()`; para toque use `on_touch()` ou
`App::is_touch_down()`/`App::last_touch()`.

### 10.6 Analógicos podem precisar de deadzone manual

`left_stick()`, `right_stick()`, `left_trigger()` e `right_trigger()`
devolvem os valores crus de `AMotionEvent_getAxisValue`. O helper não aplica
deadzone, clamp nem normalização. Gatilhos, por exemplo, podem vir com valores
diferentes de `0..1` dependendo do dispositivo; aplique seu próprio tratamento
como no exemplo da [§9](#9-exemplo-completo-quadrado-que-segue-o-stick).

### 10.7 Botões de gamepad são keycodes, não bits de motion

`button_a()`/`button_b()`/`button_x()`/`button_y()`/`button_start()` usam
`is_down(AKEYCODE_BUTTON_*)`. Se um dispositivo/reportar botões apenas no
`button state` de um motion event, o helper **não** os verá como botões (o
`mouse_buttons_` só é preenchido para source de mouse). Na prática, gamepads
Android enviam `AKEYCODE_BUTTON_*` como eventos de teclado.

### 10.8 `last_key()` não é "tecla pressionada"

Como o `last_key_` é escrito também em `AKEY_EVENT_ACTION_UP`, ele pode
apontar para uma tecla que já foi solta. Para estado use `is_down(code)`;
para "qual código chegou por último" use `last_key()`.

---

## 11. Resumo rápido

```cpp
#include <nativeapp/nativeapp.hpp>

auto& in = nativeapp::InputHelpers::instance();

// Teclado / botões de gamepad (AKEYCODE_*)
bool correndo   = in.is_down(AKEYCODE_BUTTON_A);
int32_t ultimo  = in.last_key();

// Gamepad (source GAMEPAD ou JOYSTICK)
bool conectado       = in.gamepad_connected();
nativeapp::Stick esq = in.left_stick();    // AXIS_X / AXIS_Y
nativeapp::Stick dir = in.right_stick();   // AXIS_Z / AXIS_RZ
float lt             = in.left_trigger();
float rt             = in.right_trigger();
bool start           = in.button_start();  // AKEYCODE_BUTTON_START

// Mouse (source MOUSE)
float mx = in.mouse_x(), my = in.mouse_y();
bool clique = in.mouse_down(nativeapp::MouseButton::Left);
float sx, sy;
in.consume_scroll(sx, sy);  // zera o acumulado

// Estado limpo (exceto mouse_x_/mouse_y_)
in.reset();
```

**Frases-chave para lembrar:** o `App` alimenta o helper
(`on_event()` é a primeira linha de `on_input()`); `gamepad_connected` só liga;
`reset()` limpa; scroll precisa ser consumido; teclado do sistema não é
consumido; hover não vira toque; analógico vem cru.
