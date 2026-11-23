# Sensores (`nativeapp::Sensors` / `sensor.hpp`)

> Guia completo do helper de acelerômetro/giroscópio do apkfast.
>
> **Fontes consultadas:** `include/nativeapp/sensor.hpp` (API e implementação),
> `examples/device-demo/jni/main.cpp` (exemplo funcional),
> `include/nativeapp/app.hpp` (quem faz o `poll` do looper),
> `include/nativeapp/nativeapp.hpp` (header agregador) e
> `include/nativeapp/README.md` (validação em dispositivo: `accel≈9.76`).
>
> **Regra deste documento:** só está aqui o que existe no código. Quando o
> texto tira uma consequência do código, isso aparece como **Observação**.

## Índice

1. [Visão geral](#1-visão-geral)
2. [Singleton e `init()`](#2-singleton-e-init)
3. [Filas, IDs e callbacks: por que NÃO fazer `poll` manual](#3-filas-ids-e-callbacks-por-que-não-fazer-poll-manual)
4. [`update()` e `drain()`: o que fazem de fato](#4-update-e-drain-o-que-fazem-de-fato)
5. [Valores: `Vec3f`, unidades e taxa](#5-valores-vec3f-unidades-e-taxa)
6. [`has_*`, `destroy()` e destrutor](#6-has_-destroy-e-destrutor)
7. [Tabela completa de métodos](#7-tabela-completa-de-métodos)
8. [Exemplo completo: visualizando a gravidade](#8-exemplo-completo-visualizando-a-gravidade)
9. [Armadilhas e diagnóstico](#9-armadilhas-e-diagnóstico)
10. [Resumo rápido](#10-resumo-rápido)

---

## 1. Visão geral

`Sensors` é um singleton header-only que usa a API NDK `ASensorManager` para
abrir o **sensor padrão** de acelerômetro e/ou giroscópio. A característica mais
importante do projeto está no comentário de abertura do próprio arquivo:

```cpp
// As filas são registradas no MESMO looper do App, com callback. O App::run()
// já faz o poll e despacha — este módulo NÃO consome eventos do looper.
```

Ou seja: este módulo **não roda uma bomba de eventos própria**. Ele registra
duas filas com callbacks no looper da thread atual; o `App::run()` faz o
`ALooper_pollOnce(0, ...)` e o `source->process(...)`, e é esse despacho que
chama os callbacks.

```text
 App::run()  (app.hpp)
   └─ ALooper_pollOnce(0, ...)  ──► source->process()
                                       │
                                       ▼  (looper da MESMA thread)
                       ┌─────────────────────────────────────┐
                       │ Sensors::accel_callback (id 42)     │
                       │ Sensors::gyro_callback  (id 43)     │
                       └──────────────┬──────────────────────┘
                                      │ chama
                                      ▼
                                  drain()
                                      │
                       ASensorEventQueue_getEvents(...)
                                      │
                    acceleration_ / angular_velocity_
                                      │
                   acceleration() / angular_velocity()
```

---

## 2. Singleton e `init()`

### 2.1 Singleton

```cpp
static Sensors& instance() {
    static Sensors singleton;
    return singleton;
}
```

Uma instância por processo, construtor privado (`Sensors() = default`), e o
destrutor chama `destroy()` (§6). Não existe parâmetro: ao contrário de
`Device`, não é preciso passar `JNIEnv`/Activity.

### 2.2 O que `init()` faz

```cpp
bool init() {
    if (initialized_) return true;
    manager_ = ASensorManager_getInstance();
    if (!manager_) return false;

    accelerometer_ = ASensorManager_getDefaultSensor(
        manager_, ASENSOR_TYPE_ACCELEROMETER);
    gyroscope_ = ASensorManager_getDefaultSensor(
        manager_, ASENSOR_TYPE_GYROSCOPE);
    if (!accelerometer_ && !gyroscope_) return false;

    // 0 = usa o looper da thread atual (o mesmo do App) com callbacks.
    looper_ = ALooper_prepare(0);
    if (!looper_) return false;

    if (accelerometer_) {
        accel_event_queue_ = ASensorManager_createEventQueue(
            manager_, looper_, kAccelId, &Sensors::accel_callback, this);
        if (accel_event_queue_) {
            ASensorEventQueue_enableSensor(accel_event_queue_, accelerometer_);
            ASensorEventQueue_setEventRate(
                accel_event_queue_, accelerometer_, 1000000 / 60);
        }
    }
    if (gyroscope_) { /* idem, com kGyroId e gyro_callback */ }

    initialized_ = true;
    return true;
}
```

| Passo | Chamada NDK | Resultado no código |
|---|---|---|
| 1 | `ASensorManager_getInstance()` | Guarda em `manager_`; se nulo, `init()` retorna `false` |
| 2 | `ASensorManager_getDefaultSensor(..., ASENSOR_TYPE_ACCELEROMETER)` | Guarda em `accelerometer_` (pode ser nulo) |
| 3 | `ASensorManager_getDefaultSensor(..., ASENSOR_TYPE_GYROSCOPE)` | Guarda em `gyroscope_` (pode ser nulo) |
| 4 | — | Se **ambos** forem nulos, retorna `false` |
| 5 | `ALooper_prepare(0)` | Guarda em `looper_`; se nulo, retorna `false` |
| 6 | `ASensorManager_createEventQueue(manager_, looper_, id, callback, this)` | Uma fila por sensor disponível |
| 7 | `ASensorEventQueue_enableSensor(...)` | Habilita o sensor na fila |
| 8 | `ASensorEventQueue_setEventRate(..., 1000000 / 60)` | Pede ~60 Hz |
| 9 | — | Marca `initialized_ = true` e retorna `true` |

Observações fiéis ao código:

- `init()` é **idempotente**: se `initialized_` já é `true`, devolve `true`
  imediatamente sem tocar em nada.
- `ASensorManager_getInstance()` é a função legada do NDK (marcada como
  *deprecated* em NDKs recentes, em favor de
  `ASensorManager_getInstanceForPackage`). O código **usa exatamente essa
  função**; não há chamada a `getInstanceForPackage` nem consulta de permissão
  de sensores em `sensor.hpp`.
- `ALooper_prepare(0)` usa o looper **da thread chamadora** — o `0` é o
  comentário literal do header: "usa o looper da thread atual (o mesmo do
  App) com callbacks". Por isso a inicialização deve acontecer na thread do
  `App` (tipicamente em `on_ready()`), depois que o `App` existe. Ver §9.1.
- `ASensorEventQueue_enableSensor()` e `ASensorEventQueue_setEventRate()` têm o
  retorno **ignorado**. Não há log algum em `sensor.hpp` (embora
  `<android/log.h>` seja incluído).
- É possível `init()` retornar `true` com apenas **um** dos dois sensores
  presentes — basta que pelo menos um `getDefaultSensor` tenha dado não nulo.
- Se `manager_` estiver nulo, o código retorna antes de `initialized_ = true`;
  portanto uma chamada posterior de `init()` tenta de novo.

### 2.3 Onde chamar

O exemplo `device-demo` chama em `on_ready()` (junto com `Device` e `Audio`):

```cpp
void on_ready() override {
    // ...
    LOGI("sensores: accel=%d gyro=%d init=%d",
         Sensors::instance().init(),
         Sensors::instance().has_gyroscope(),
         Sensors::instance().has_accelerometer());
    // ...
}
```

> **Observação:** nesse `LOGI` a ordem de avaliação dos argumentos em C++ não
> é especificada. `has_gyroscope()` / `has_accelerometer()` podem ser lidos
> **antes** de `init()` rodar, e o log pode mostrar `0` mesmo com sensores
> presentes. Separe em instruções (como no exemplo do §8).

---

## 3. Filas, IDs e callbacks: por que NÃO fazer `poll` manual

### 3.1 Os IDs e os callbacks

```cpp
private:
    static constexpr int kAccelId = 42;
    static constexpr int kGyroId  = 43;

    static int accel_callback(int, int, void* data) {
        static_cast<Sensors*>(data)->drain();
        return 1;
    }
    static int gyro_callback(int, int, void* data) {
        static_cast<Sensors*>(data)->drain();
        return 1;
    }
```

| Item | Valor / comportamento |
|---|---|
| `kAccelId` | `42` — identificador da fila do acelerômetro no looper |
| `kGyroId` | `43` — identificador da fila do giroscópio no looper |
| `accel_callback(int, int, void*)` | Faz o cast de `data` para `Sensors*` e chama `drain()`; retorna `1` |
| `gyro_callback(int, int, void*)` | Mesma coisa; retorna `1` |
| `data` passado | `this`, enviado como último argumento de `createEventQueue` |

Os dois callbacks são idênticos: **ambos drenam as duas filas**. Isso é
intencional e inofensivo — se a fila do outro sensor estiver vazia,
`ASensorEventQueue_getEvents` retorna `0` e nada muda.

### 3.2 A regra: não use seu próprio `poll`

O `App::run()` já faz isso em loop (`app.hpp`):

```cpp
while (ALooper_pollOnce(0, nullptr, &events,
                        reinterpret_cast<void**>(&source)) >= 0) {
    if (source) source->process(app_, source);
    // ...
}
```

É o `source->process()` que despacha o callback registrado com o ID `42`/`43`.
Se você criar outro `ALooper_pollOnce`/`ALooper_pollAll` para "pegar os
eventos dos sensores", você passa a competir com o `App` pelo **mesmo looper** e
pode consumir eventos que pertencem ao ciclo de vida/input do app. O comentário
do header e o comentário de `update()` são explícitos: o módulo não consome
eventos do looper e `update()` "não faz mais poll (evita roubar eventos do
App)".

| Prática | O que acontece no código |
|---|---|
| Deixar o `App` girar o looper | ✔ Os callbacks `42`/`43` disparam e `drain()` atualiza os caches |
| Chamar `Sensors::update()` a cada frame | Inofensivo, mas **redundante**: só reexecuta `drain()`; normalmente não há eventos pendentes porque o callback já os consumiu |
| Chamar `Sensors::drain()` manualmente junto com um `poll` próprio | ✘ Risco de drenar a fila antes do despacho do App / disputar o mesmo looper |
| Fazer `ALooper_pollOnce` extra "só para sensores" | ✘ Rouba eventos do `App`; os thunks de comando/input podem não receber o que precisam |
| Chamar `init()` em outra thread | ✘ `ALooper_prepare(0)` pega outro looper, que o `App::run()` não faz `poll`; os callbacks nunca disparam (§9.1) |

> **Observação:** não existe mutex em `Sensors`. `drain()` é executado dentro
> do despacho do looper da thread do `App`, e as leituras de `acceleration()` /
> `angular_velocity()` devem acontecer nessa mesma thread (ou com sua própria
> sincronização) — o header não protege acesso concorrente.

---

## 4. `update()` e `drain()`: o que fazem de fato

```cpp
/// Chamado automaticamente pelo callback do looper — não precisa chamar.
void drain() {
    if (accel_event_queue_) {
        ASensorEvent event;
        while (ASensorEventQueue_getEvents(accel_event_queue_, &event, 1) > 0) {
            acceleration_ = {event.vector.x, event.vector.y, event.vector.z};
        }
    }
    if (gyro_event_queue_) {
        ASensorEvent event;
        while (ASensorEventQueue_getEvents(gyro_event_queue_, &event, 1) > 0) {
            angular_velocity_ = {event.vector.x, event.vector.y, event.vector.z};
        }
    }
}

/// Compatibilidade: não faz mais poll (evita roubar eventos do App).
void update() { drain(); }
```

| Detalhe | Comportamento real |
|---|---|
| `getEvents(queue, &event, 1)` | Lê **um evento por chamada**, em `while` até a fila esvaziar |
| Múltiplos eventos pendentes | Cada iteração **sobrescreve** o cache: vale o **último** evento drenado |
| Média / filtro / timestamp | **Não existe** — o código usa só `event.vector.x/y/z` |
| Fila nula | Simplesmente ignorada (sensor ausente ou `init()` não chamado) |
| `update()` | Apenas `drain()`; nenhum `poll`, nenhuma chamada NDK extra |
| Retorno dos callbacks | `1` (evento tratado) |

O `device-demo` mantém a chamada em `on_tick()` por compatibilidade:

```cpp
void on_tick() override {
    Sensors::instance().update();          // redundante, mas inofensivo
    if (frames() % 120 == 0) {
        const auto accel = Sensors::instance().acceleration();
        LOGI("accel=%.2f %.2f %.2f ...", accel.x, accel.y, accel.z, ...);
    }
}
```

> **Observação:** se você sobrescrever `on_tick()`, chame `App::on_tick()`
> antes da sua lógica — é a implementação base que roda
> `Permissions::instance(app_->activity).update()` (ver `docs/app.md`).
> Alternativamente, use `on_update(float)` para a lógica de frame, como no
> exemplo do §8.

---

## 5. Valores: `Vec3f`, unidades e taxa

### 5.1 `Vec3f`

```cpp
struct Vec3f { float x = 0, y = 0, z = 0; };
```

É um struct simples, com os três componentes inicializados em zero. Tanto
`acceleration()` quanto `angular_velocity()` devolvem **`Vec3f` por valor**:

```cpp
Vec3f acceleration() const { return acceleration_; }
Vec3f angular_velocity() const { return angular_velocity_; }
```

Antes de `init()` (ou sem evento recebido), o valor é `{0, 0, 0}`.

### 5.2 Unidades

O código copia `event.vector.x`, `.y` e `.z` **sem conversão**. Pelas
definições da API NDK para os tipos pedidos:

| Sensor | Tipo pedido | Campo copiado | Unidade |
|---|---|---|---|
| Acelerômetro | `ASENSOR_TYPE_ACCELEROMETER` | `event.vector.*` | **m/s²** (inclui a gravidade) |
| Giroscópio | `ASENSOR_TYPE_GYROSCOPE` | `event.vector.*` | **rad/s** (velocidade angular) |

Consequências práticas:

- Em repouso sobre a mesa, a magnitude do vetor de aceleração fica perto de
  **9,81 m/s²** (o `README.md` registra `accel≈9.76` no dispositivo validado).
  **Não divida por 9,81** esperando "g": o helper não faz essa conversão.
- O eixo `z` normalmente carrega a gravidade quando o aparelho está deitado de
  costas; a orientação dos eixos é a do sensor do dispositivo.
- O giroscópio devolve rad/s; para graus/s, multiplique por `180/π` no seu
  código.

### 5.3 Taxa de 60 Hz

```cpp
ASensorEventQueue_setEventRate(accel_event_queue_, accelerometer_, 1000000 / 60);
ASensorEventQueue_setEventRate(gyro_event_queue_, gyroscope_, 1000000 / 60);
```

O argumento é em **microsegundos**. `1000000 / 60` é divisão inteira C++:
`16666` µs por evento, ou seja, uma solicitação de aproximadamente **60 Hz**
(60,0012 Hz se calculado de volta). É um valor pedido ao sistema; o retorno de
`setEventRate()` é ignorado e o código não verifica a taxa efetiva.

---

## 6. `has_*`, `destroy()` e destrutor

```cpp
bool has_accelerometer() const { return accelerometer_ != nullptr; }
bool has_gyroscope() const { return gyroscope_ != nullptr; }
```

| Método | Retorno |
|---|---|
| `has_accelerometer()` | `accelerometer_ != nullptr` |
| `has_gyroscope()` | `gyroscope_ != nullptr` |

São consultas ao **ponteiro do sensor**, não à fila. Isso importa em dois casos:

1. Antes de `init()`, ambos devolvem `false`.
2. Depois de `destroy()`, ambos devolvem `false`, porque `destroy()` zera os
   ponteiros (§9.3).
3. Se o sensor existe mas `createEventQueue()` falhou, `has_*()` ainda devolve
   `true` — mas a fila correspondente é nula e o cache (`acceleration_` ou
   `angular_velocity_`) nunca é atualizado. O código não expõe as filas.

### `destroy()`

```cpp
void destroy() {
    if (accel_event_queue_) {
        ASensorManager_destroyEventQueue(manager_, accel_event_queue_);
        accel_event_queue_ = nullptr;
    }
    if (gyro_event_queue_) {
        ASensorManager_destroyEventQueue(manager_, gyro_event_queue_);
        gyro_event_queue_ = nullptr;
    }
    accelerometer_ = nullptr;
    gyroscope_ = nullptr;
    looper_ = nullptr;
    manager_ = nullptr;
    initialized_ = false;
}

~Sensors() { destroy(); }
```

| Campo | Depois de `destroy()` |
|---|---|
| `accel_event_queue_`, `gyro_event_queue_` | Destruídos (se existiam) e postos em `nullptr` |
| `accelerometer_`, `gyroscope_` | `nullptr` |
| `looper_`, `manager_` | `nullptr` |
| `initialized_` | `false` |
| `acceleration_`, `angular_velocity_` | **Não são zerados** — permanece o último valor lido |

> **Observação:** `destroy()` é idempotente (tudo é checado antes de usar). Como
> `initialized_` volta a `false`, uma nova `init()` na mesma instância refaz o
> processo. O destrutor do singleton também chama `destroy()`.

> **Observação:** o `App::finish()` **não** chama `Sensors::destroy()` — ele só
> desliga `Permissions`/`JniPermissions` e chama `on_destroy()`. Se você precisa
> liberar as filas antes do fim do processo, faça explicitamente (por exemplo
> no seu `on_destroy()`).

---

## 7. Tabela completa de métodos

### Públicos

| Assinatura real | Categoria | Comportamento |
|---|---|---|
| `static Sensors& instance()` | Singleton | Instância estática local; construtor privado |
| `bool init()` | Inicialização | Idempotente; abre manager, sensores padrão, looper atual e as filas; `false` se não há manager, nenhum sensor ou sem looper |
| `void drain()` | Eventos | Drena as duas filas com `getEvents(..., 1)` até esvaziar; **chamado pelos callbacks** |
| `void update()` | Compatibilidade | Apenas chama `drain()`; não faz `poll` |
| `Vec3f acceleration() const` | Leitura | Cópia do cache do acelerômetro (m/s²) |
| `Vec3f angular_velocity() const` | Leitura | Cópia do cache do giroscópio (rad/s) |
| `bool has_accelerometer() const` | Capacidade | `accelerometer_ != nullptr` |
| `bool has_gyroscope() const` | Capacidade | `gyroscope_ != nullptr` |
| `void destroy()` | Limpeza | Destroi filas, zera ponteiros e `initialized_` |
| `~Sensors()` | Limpeza | Chama `destroy()` |

### Privados (contexto, não são API)

| Membro | Valor / papel |
|---|---|
| `kAccelId = 42` | Identificador da fila do acelerômetro no looper |
| `kGyroId = 43` | Identificador da fila do giroscópio no looper |
| `accel_callback(int, int, void*)` | `static_cast<Sensors*>(data)->drain(); return 1;` |
| `gyro_callback(int, int, void*)` | Idem |
| `manager_`, `accelerometer_`, `gyroscope_` | Estado NDK |
| `accel_event_queue_`, `gyro_event_queue_`, `looper_` | Estado das filas |
| `acceleration_`, `angular_velocity_` | Últimos valores lidos |
| `initialized_` | Guarda da idempotência de `init()` |

Não existem, no header: `is_initialized()`, `set_event_rate()`, calibração,
filtragem, timestamp, callback do usuário, rumo/azimute ou trigger de shake.

---

## 8. Exemplo completo: visualizando a gravidade

Baseado em `examples/device-demo/jni/main.cpp`, com foco em sensores. O quadrado
é deslocado por `accel.x/accel.y` e sua cor fica mais intensa conforme a
magnitude do vetor (perto de 9,81 m/s² em repouso).

```cpp
// jni/main.cpp — sensores: acelerômetro/giroscópio + visualização da gravidade
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#include <cmath>

#define LOG_TAG "sensors-demo"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class SensorsDemo : public App {
public:
    explicit SensorsDemo(android_app* app) : App(app) {}

private:
    void on_ready() override {
        // A inicialização vai na thread do App (on_ready roda dentro do App).
        const bool ok = Sensors::instance().init();
        const bool accel = Sensors::instance().has_accelerometer();
        const bool gyro  = Sensors::instance().has_gyroscope();
        LOGI("sensors init=%d accel=%d gyro=%d", ok, accel, gyro);

        // Não é preciso chamar update()/drain(): os callbacks (ids 42/43)
        // já drenam as filas quando o App faz o poll do looper.

        set_scene(&scene_);  // App chama begin_frame()/present()
        scene_.set_clear_color(0.05f, 0.05f, 0.10f);
    }

    void on_surface_created() override { scene_.init(window()); }
    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_update(float) override {
        if (frames() % 120 != 0) return;

        const Vec3f a = Sensors::instance().acceleration();
        const Vec3f g = Sensors::instance().angular_velocity();
        const float mag = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        LOGI("accel(m/s^2)=%.2f %.2f %.2f |a|=%.2f gyro(rad/s)=%.3f %.3f %.3f",
             a.x, a.y, a.z, mag, g.x, g.y, g.z);
    }

    void on_draw() override {
        const Vec3f a = Sensors::instance().acceleration();

        // Desloca o quadrado conforme a inclinação do aparelho.
        const float x = width()  / 2.0f + a.x * 20.0f;
        const float y = height() / 2.0f + a.y * 20.0f;

        Mat4 model = mat4_translate(x, y, 0.0f);
        model = mat4_multiply(model, mat4_scale(120.0f, 120.0f, 1.0f));

        const float mag = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        Material material;
        material.color[0] = 0.25f + mag / 20.0f;  // mais/branco com gravidade
        material.color[1] = 0.45f;
        material.color[2] = 1.00f;
        material.color[3] = 1.00f;
        scene_.draw(tile_, material, model.data());
    }

    void on_touch(TouchAction action,
                  const std::vector<TouchPoint>& pts) override {
        if (action == TouchAction::Down && !pts.empty()) {
            const Vec3f a = Sensors::instance().acceleration();
            LOGI("toque: accel=%.2f %.2f %.2f", a.x, a.y, a.z);
        }
    }

    void on_key(int32_t code, bool down) override {
        if (!down) return;
        if (code == 4) {                       // BACK
            Sensors::instance().destroy();     // opcional
            request_exit();
        }
    }

    GLES2Scene scene_;
    MeshData tile_ = MeshData::quad();
};

APP_MAIN(SensorsDemo)
```

Pontos a notar no exemplo:

- `init()`, `has_accelerometer()` e `has_gyroscope()` ficam em instruções
  separadas (evita a armadilha de ordem de avaliação do `LOGI` do
  `device-demo`).
- Não há `update()`/`drain()` manual nem `poll` extra.
- `on_update(float)` é usado em vez de `on_tick()`, para não sobrescrever o
  pump de permissões da base.
- `destroy()` em `on_key()` apenas ilustra a API; o destrutor do singleton já
  faria isso no fim do processo.

---

## 9. Armadilhas e diagnóstico

### 9.1 Inicializar antes do `App` (ou na thread errada)

`init()` chama `ALooper_prepare(0)`, que pega o looper da **thread atual**. O
`App::run()` é quem gira esse looper (`ALooper_pollOnce(0, ...)` +
`source->process(...)`). Chamar `Sensors::instance().init()` antes de o `App`
existir — por exemplo em um objeto global, ou no `android_main` antes de
`run()` — ou chamar em outra thread pode registrar as filas em um looper que
ninguém faz `poll`. Sintoma: `init()` retorna `true`, `has_*()` retorna `true`,
mas `acceleration()` fica para sempre `{0,0,0}`.

**Faça** em `on_ready()` (ou outro callback da thread do App).

### 9.2 `update()`/`drain()` com `poll` manual

Repetido por ser o ponto mais importante: os callbacks com IDs `42`/`43`
já chamam `drain()` dentro do despacho do `App`. `update()` existe apenas para
compatibilidade e não faz `poll`. Adicionar seu próprio `ALooper_pollOnce`
para sensores disputa o mesmo looper e pode roubar eventos do App. Use
`acceleration()`/`angular_velocity()` diretamente.

### 9.3 Confiar em `has_*()` depois de `destroy()`

`destroy()` zera `accelerometer_` e `gyroscope_`, então os dois `has_*()`
passam a devolver `false` — mesmo que o dispositivo tenha os sensores. Isso
representa "não há sensor registrado nesta instância agora", não "o hardware
desapareceu". Curiosamente, `acceleration_`/`angular_velocity_` **não** são
zerados: depois de `destroy()` as Leituras ainda devolvem o último valor.

### 9.4 Sensores ausentes / falha parcial

| Situação | `init()` | `has_accelerometer()` | `has_gyroscope()` |
|---|---|---|---|
| Nenhum dos dois existe | `false` | `false` | `false` |
| Só acelerômetro | `true` | `true` | `false` |
| Só giroscópio | `true` | `false` | `true` |
| Sensor existe, mas `createEventQueue()` falhou | `true` | `true` (ponteiro existe) | depende |
| `manager_`/looper nulos | `false` | depende do que já foi atribuído | depende |

Sempre verifique `has_*()` **depois** de `init()` e trate `{0,0,0}` como
"ainda sem dado" — é indistinguível de "sensor presente, mas sem evento".

### 9.5 Callback no mesmo looper do App

Os callbacks retornam `1` e fazem `drain()` **dentro** do
`source->process()` do App. Não faça trabalho longo, bloqueante ou JNI pesado
dentro deles (eles não são seus — são da classe). Não tente "despachar"
manualmente esses IDs: o `App` já faz isso. Também não existe controle de
concorrência (§3.2).

### 9.6 `ASensorManager_getInstance()` é a API legada

O código usa `ASensorManager_getInstance()`, função marcada como *deprecated*
em NDKs recentes. Isso **compila e funciona** neste projeto, mas não há aqui
uma versão com `ASensorManager_getInstanceForPackage`, nem solicitação de
permissão `android.permission.HIGH_SAMPLING_RATE_SENSORS` — o helper apenas
pede `1000000 / 60` µs via `setEventRate()`.

### 9.7 Falhas silenciosas e log enganoso

`_sensor.hpp` inclui `<android/log.h>` mas **não loga nada**. Falhas de
`getInstance`, `getDefaultSensor`, `ALooper_prepare`, `enableSensor` e
`setEventRate` são silenciosas; o único sinal é o retorno de `init()` e os
`has_*()`. E, como visto em §2.3, um `LOGI` com `init()` e `has_*()` na mesma
chamada pode exibir valores "pré-init" por causa da ordem de avaliação.

### 9.8 O `App` não desliga os sensores por você

`App::finish()` não chama `Sensors::destroy()`. Se você recria o looper ou
quer liberar as filas antes de sair, chame `destroy()` no seu `on_destroy()`.
O singleton será destruído de todo modo no encerramento do processo.

### 9.9 Diagnóstico rápido

| Sintoma | Causa provável no código |
|---|---|
| `init()==true`, vetores sempre zero | `init()` fora da thread do App; callbacks não despachados (§9.1) |
| `init()==false` com `has_*()` `false` | Nenhum sensor padrão, manager nulo ou `ALooper_prepare(0)` falhou |
| Vetor muda só de vez em quando, "atrasado" | Cache é o **último** evento drenado; `drain()` não interpola |
| Gravidade lida como ~1 em vez de ~9,8 | Você converteu para `g`; o helper entrega **m/s²** |
| Eventos do App some/parcialmente | `poll` manual disputando o looper (§9.2) |
| `has_*()` virou `false` do nada | `destroy()` foi chamado (direta ou pelo destrutor) |

---

## 10. Resumo rápido

```text
App (on_ready, thread do App)
  └─ Sensors::instance().init()
       ├─ ASensorManager_getInstance()
       ├─ getDefaultSensor(ACCELEROMETER) / (GYROSCOPE)
       ├─ ALooper_prepare(0)         → looper da thread atual
       ├─ createEventQueue(id 42 / 43, callback, this)
       ├─ enableSensor(...)
       └─ setEventRate(1000000 / 60) → ~60 Hz

App::run() → ALooper_pollOnce → source->process
                                  └─ accel_callback / gyro_callback
                                       └─ drain() → getEvents → caches

Leitura:  acceleration()  → Vec3f em m/s²
          angular_velocity() → Vec3f em rad/s

NÃO faça: poll próprio, update()/drain() para "puxar" eventos,
          init() fora da thread do App.

Limpeza: destroy() (ou ~Sensors()) zera filas e ponteiros;
         App NÃO chama destroy() automaticamente.
```
