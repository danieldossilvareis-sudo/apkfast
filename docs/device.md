# Dispositivo (`nativeapp::Device` / `device.hpp`)

> Guia completo do helper de utilidades do dispositivo do apkfast.
>
> **Fontes consultadas:** `include/nativeapp/device.hpp` (API e implementação),
> `examples/device-demo/jni/main.cpp` (exemplo funcional),
> `examples/device-demo/AndroidManifest.xml` (permissão `VIBRATE`),
> `include/nativeapp/app.hpp` (o que o `App` inicializa — e o que não inicializa),
> `include/nativeapp/nativeapp.hpp` (header agregador) e
> `apkfast/scaffold.py` (`COMMON_PERMISSIONS`).
>
> **Regra deste documento:** só está aqui o que existe no código. Quando o
> texto tira uma consequência do código, isso aparece como **Observação**.

## Índice

1. [Visão geral](#1-visão-geral)
2. [Singleton e inicialização](#2-singleton-e-inicialização)
3. [`vibrate(ms)`](#3-vibratems)
4. [Bateria: `battery_level()` e `is_charging()`](#4-bateria-battery_level-e-is_charging)
5. [Orientação: `orientation()`](#5-orientação-orientation)
6. [Identidade do aparelho: `device_model()`, `android_version()`, `api_level()`](#6-identidade-do-aparelho-device_model-android_version-api_level)
7. [Brilho: `set_brightness()`](#7-brilho-set_brightness)
8. [Tabela completa de métodos](#8-tabela-completa-de-métodos)
9. [Como a API lida com `jclass`/`jmethodID` nulos](#9-como-a-api-lida-com-jclassjmethodid-nulos)
10. [Exemplo completo](#10-exemplo-completo)
11. [Armadilhas e diagnóstico](#11-armadilhas-e-diagnóstico)
12. [Resumo rápido](#12-resumo-rápido)

---

## 1. Visão geral

`Device` é um singleton baseado em JNI que expõe utilidades do aparelho sem
classes Java próprias: vibração, nível/estado de bateria, orientação da janela,
modelo/versão do Android, nível de API e brilho da janela.

```text
 Device::instance(env, activity)   ← chamador DEVE passar JNIEnv + Activity
        │  guarda env_ / activity_
        ├── vibrate(ms)            → Context.getSystemService("vibrator")
        │                            → Vibrator.vibrate(long)
        ├── battery_level()        → registerReceiver(null, IntentFilter(BATTERY_CHANGED))
        ├── is_charging()          → intent.getIntExtra("plugged", 0)
        ├── orientation()          → WindowManager.getDefaultDisplay().getRotation()
        ├── device_model()         → Build.MODEL
        ├── android_version()      → Build$VERSION.RELEASE
        ├── api_level()            → Build$VERSION.SDK_INT
        └── set_brightness(v)      → Activity.getWindow().getAttributes()
                                      → LayoutParams.screenBrightness = clamp(v)
                                      → Window.setAttributes()
```

> **Ponto crítico:** o `App` **não inicializa `Device` automaticamente**. Ele
> injeta `AAssetManager` em `Assets` e `JNIEnv`/Activity em `TextInput`, mas
> nunca chama `Device::instance(...)`. Cabe ao chamador obter um `JNIEnv*`
> válido e passar `JNIEnv` + Activity pelo menos uma vez. Ver §2 e §11.2.

---

## 2. Singleton e inicialização

### 2.1 `instance(JNIEnv*, jobject activity)`

```cpp
static Device& instance(JNIEnv* env, jobject activity) {
    static Device* singleton = nullptr;
    if (!singleton) singleton = new Device(env, activity);
    if (env && activity) { singleton->env_ = env; singleton->activity_ = activity; }
    return *singleton;
}
```

| Aspecto | Comportamento real |
|---|---|
| Tipo de singleton | Ponteiro estático alocado com `new` na primeira chamada (não é um objeto local estático) |
| Construtor | Privado: `explicit Device(JNIEnv* env, jobject activity) : env_(env), activity_(activity) {}` |
| Primeira chamada com `env`/`activity` válidos | Cria o singleton já com as refs guardadas |
| Primeira chamada com `nullptr, nullptr` | Cria o singleton com `env_`/`activity_` nulos → todos os métodos caem no caminho de "indisponível" até uma chamada posterior com refs válidas |
| Chamadas seguintes com argumentos válidos | **Atualizam** `env_` e `activity_` |
| Chamadas seguintes com `nullptr, nullptr` | Não alteram nada; devolvem o singleton existente com as refs já guardadas |
| Destrutor | **Não existe** `~Device()`; o `new` nunca é acompanhado de `delete` no header |

É por isso que o `device-demo` pode usar `Device::instance(nullptr, nullptr)`
depois de ter inicializado o singleton uma vez:

```cpp
void on_ready() override {
    JNIEnv* env = nullptr;
    activity()->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (!env) activity()->vm->AttachCurrentThread(&env, nullptr);
    Device::instance(env, activity()->clazz);   // <- única inicialização necessária

    LOGI("device=%s android=%s api=%d bateria=%d%%",
         Device::instance(nullptr, nullptr).device_model().c_str(),
         Device::instance(nullptr, nullptr).android_version().c_str(),
         Device::instance(nullptr, nullptr).api_level(),
         Device::instance(nullptr, nullptr).battery_level());
}
```

> **Observação:** `activity()->clazz` é o `jobject` da Activity nativa
> (`ANativeActivity::clazz`), passado diretamente como `activity_`. O singleton
> guarda o ponteiro recebido; ele não cria uma referência global nova nem
> chama `NewGlobalRef`.

### 2.2 JNIEnv da thread correta

O exemplo obtém o env com `vm->GetEnv(..., JNI_VERSION_1_6)` e, se não houver
env na thread, chama `vm->AttachCurrentThread(&env, nullptr)`. Isso é feito em
`on_ready()`, que roda na thread nativa do `App`.

Todos os métodos de `Device` usam `env_` para `FindClass`, `GetMethodID`,
`NewStringUTF`, `Call*Method` e `DeleteLocalRef`. Se o `JNIEnv*` guardado não
pertencer à thread chamadora, essas chamadas não são válidas. Ver §11.2.

---

## 3. `vibrate(ms)`

```cpp
/// Vibra por `millis` (requer android.permission.VIBRATE).
void vibrate(int millis) {
    if (!env_ || !activity_) return;
    jobject service = system_service("vibrator");
    if (!service) return;
    jclass clazz = env_->FindClass("android/os/Vibrator");
    if (clazz) {
        jmethodID vibrate = env_->GetMethodID(clazz, "vibrate", "(J)V");
        if (vibrate) {
            env_->CallVoidMethod(service, vibrate, static_cast<jlong>(millis));
        }
        env_->DeleteLocalRef(clazz);
    }
    env_->DeleteLocalRef(service);
}
```

| Etapa | Código |
|---|---|
| Guarda | `!env_ \|\| !activity_` → retorno silencioso |
| Serviço | `system_service("vibrator")` → `Context.getSystemService(String)` |
| Classe | `FindClass("android/os/Vibrator")` |
| Método | `GetMethodID(clazz, "vibrate", "(J)V")` |
| Chamada | `CallVoidMethod(service, vibrate, static_cast<jlong>(millis))` |
| Limpeza | `DeleteLocalRef(clazz)` e `DeleteLocalRef(service)` |

**Permissão:** o comentário do header exige
`android.permission.VIBRATE`. O manifesto do `device-demo` declara:

```xml
<uses-permission android:name="android.permission.VIBRATE" />
```

O `apkfast/scaffold.py` também inclui `android.permission.VIBRATE` em
`COMMON_PERMISSIONS`, então projetos gerados pelo assistente já a declaram.
Sem a tag no manifesto, a chamada JNI existe mas o sistema não tem a permissão
registrada para o pacote (ver `docs/permissions.md`, §9).

> **Observação:** o código não valida `millis` (valores negativos são
> convertidos para `jlong` e repassados). O serviço nulo é verificado, mas
> `FindClass` e `GetMethodID` não são: se algo falhar, o método simplesmente
> não vibra e não registra nada em log.

> **Observação:** `vibrate(J)` é a assinatura pedida; o código não faz
> checagem de API nem usa canais/amplitudes.

Uso no `device-demo`:

```cpp
void on_ready() override {
    // ...
    Device::instance(nullptr, nullptr).vibrate(80);   // ao iniciar
}

void on_touch(TouchAction action, const std::vector<TouchPoint>& pts) override {
    if (action == TouchAction::Down && !pts.empty()) {
        Device::instance(nullptr, nullptr).vibrate(40);  // por toque
    }
}
```

---

## 4. Bateria: `battery_level()` e `is_charging()`

Ambos usam o mesmo auxiliar privado:

```cpp
jobject register_receiver(const char* action) const {
    if (!env_ || !activity_) return nullptr;
    jclass intent_filter = find_class("android/content/IntentFilter");
    if (!intent_filter) return nullptr;
    jmethodID ctor = env_->GetMethodID(intent_filter, "<init>", "(Ljava/lang/String;)V");
    if (!ctor) { env_->DeleteLocalRef(intent_filter); return nullptr; }
    jstring action_str = env_->NewStringUTF(action);
    jobject filter = env_->NewObject(intent_filter, ctor, action_str);
    env_->DeleteLocalRef(action_str);

    jclass ctx = find_class("android/content/Context");
    if (!ctx) { env_->DeleteLocalRef(intent_filter); return nullptr; }
    jmethodID register_method = env_->GetMethodID(
        ctx, "registerReceiver",
        "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)"
        "Landroid/content/Intent;");
    env_->DeleteLocalRef(ctx);
    if (!register_method) { env_->DeleteLocalRef(intent_filter); return nullptr; }
    jobject intent = env_->CallObjectMethod(
        activity_, register_method, nullptr, filter);
    env_->DeleteLocalRef(filter);
    env_->DeleteLocalRef(intent_filter);
    return intent;
}
```

O `receiver` passado é **`nullptr`**. Isso pede ao Android o *sticky Intent*
de `BATTERY_CHANGED`, sem registrar um `BroadcastReceiver` de verdade — por
isso não há `unregisterReceiver()` correspondente no código.

### 4.1 `battery_level()`

```cpp
int battery_level() const {
    if (!env_ || !activity_) return -1;
    jobject intent = register_receiver("android.intent.action.BATTERY_CHANGED");
    if (!intent) return -1;
    jclass clazz = env_->GetObjectClass(intent);
    jmethodID get_int = env_->GetMethodID(clazz, "getIntExtra", "(Ljava/lang/String;I)I");
    jstring key = env_->NewStringUTF("level");
    jstring max_key = env_->NewStringUTF("scale");
    const int level = env_->CallIntMethod(intent, get_int, key, 0);
    const int scale = env_->CallIntMethod(intent, get_int, max_key, 100);
    env_->DeleteLocalRef(key);
    env_->DeleteLocalRef(max_key);
    env_->DeleteLocalRef(clazz);
    env_->DeleteLocalRef(intent);
    return scale > 0 ? (level * 100) / scale : -1;
}
```

| Entrada do Intent | Default | Uso |
|---|---|---|
| `"level"` | `0` | Numerador |
| `"scale"` | `100` | Denominador |

Resultado: `scale > 0 ? (level * 100) / scale : -1`. A divisão é **inteira**,
com truncamento; `-1` também é o valor de "indisponível" (sem env/activity ou
sem Intent).

### 4.2 `is_charging()`

```cpp
bool is_charging() const {
    if (!env_ || !activity_) return false;
    jobject intent = register_receiver("android.intent.action.BATTERY_CHANGED");
    if (!intent) return false;
    // ... getIntExtra("plugged", 0) ...
    return plugged != 0;
}
```

| Valor de `plugged` | Retorno |
|---|---|
| `0` (não carregando) | `false` |
| Qualquer não-zero | `true` |

O código não distingue AC/USB/sem fio: qualquer valor diferente de zero conta
como carregando. Em caso de indisponibilidade devolve `false`.

> **Observação:** cada chamada monta um `IntentFilter`, chama
> `registerReceiver` e libera as refs locais. Não é um cache: chamar
> `battery_level()` e `is_charging()` no mesmo frame faz duas buscas ao sticky
> Intent. Evite em `on_draw()`/`on_update()` a cada frame (§11.5).

---

## 5. Orientação: `orientation()`

```cpp
enum class Orientation { Portrait, Landscape, Unknown };
```

```cpp
Orientation orientation() const {
    if (!env_ || !activity_) return Orientation::Unknown;
    // getRotation() reflete a rotação real da janela mesmo em multi-window.
    jclass context_class = find_class("android/content/Context");
    if (!context_class) return Orientation::Unknown;
    jmethodID get_wm = env_->GetMethodID(
        context_class, "getWindowManager", "()Landroid/view/WindowManager;");
    env_->DeleteLocalRef(context_class);
    if (!get_wm) return Orientation::Unknown;

    jobject wm = env_->CallObjectMethod(activity_, get_wm);
    if (!wm) return Orientation::Unknown;
    jclass wm_class = env_->GetObjectClass(wm);
    jmethodID get_display = wm_class
        ? env_->GetMethodID(wm_class, "getDefaultDisplay", "()Landroid/view/Display;")
        : nullptr;
    jobject display = get_display ? env_->CallObjectMethod(wm, get_display) : nullptr;
    Orientation result = Orientation::Unknown;
    if (display) {
        jclass display_class = env_->GetObjectClass(display);
        jmethodID get_rotation = display_class
            ? env_->GetMethodID(display_class, "getRotation", "()I") : nullptr;
        if (get_rotation) {
            const int rotation = env_->CallIntMethod(display, get_rotation);
            // ROTATION_0=0, 90=1, 180=2, 270=3
            result = (rotation == 1 || rotation == 3)
                ? Orientation::Landscape : Orientation::Portrait;
        }
        if (display_class) env_->DeleteLocalRef(display_class);
        env_->DeleteLocalRef(display);
    }
    if (wm_class) env_->DeleteLocalRef(wm_class);
    env_->DeleteLocalRef(wm);
    return result;
}
```

Caminho percorrido: `Context.getWindowManager()` →
`WindowManager.getDefaultDisplay()` → `Display.getRotation()`.

| `getRotation()` | Constante Android | Retorno |
|---|---|---|
| `0` | `ROTATION_0` | `Portrait` |
| `1` | `ROTATION_90` | `Landscape` |
| `2` | `ROTATION_180` | `Portrait` |
| `3` | `ROTATION_270` | `Landscape` |
| Falha em qualquer etapa JNI | — | `Unknown` |

O comentário no código registra que `getRotation()` reflete a rotação real da
janela mesmo em **multi-window**. Não há distinção entre retrato normal e
invertido (ambos viram `Portrait`), nem entre paisagens (ambos `Landscape`).
`Unknown` é usado **somente** quando alguma etapa falha, não como rotação
válida.

Uso típico:

```cpp
switch (Device::instance(nullptr, nullptr).orientation()) {
    case Device::Orientation::Portrait:  /* ... */ break;
    case Device::Orientation::Landscape: /* ... */ break;
    case Device::Orientation::Unknown:   /* ... */ break;
}
```

---

## 6. Identidade do aparelho: `device_model()`, `android_version()`, `api_level()`

Os dois primeiros usam o auxiliar privado `build_string_field`:

```cpp
std::string build_string_field(const char* class_name, const char* key) const {
    if (!env_) return {};
    jclass build = find_class(class_name);
    if (!build) return {};
    jfieldID field = env_->GetStaticFieldID(build, key, "Ljava/lang/String;");
    if (!field) { env_->DeleteLocalRef(build); return {}; }
    jstring value = static_cast<jstring>(env_->GetStaticObjectField(build, field));
    std::string result;
    if (value) {
        const char* chars = env_->GetStringUTFChars(value, nullptr);
        result = chars ? chars : "";
        if (chars) env_->ReleaseStringUTFChars(value, chars);
        env_->DeleteLocalRef(value);
    }
    env_->DeleteLocalRef(build);
    return result;
}
```

| Método | Classe / campo | Retorno em falha |
|---|---|---|
| `std::string device_model() const` | `android/os/Build` → `MODEL` (`Ljava/lang/String;`) | `""` (string vazia) |
| `std::string android_version() const` | `android/os/Build$VERSION` → `RELEASE` | `""` |
| `int api_level() const` | `android/os/Build$VERSION` → `SDK_INT` (`I`) | `0` |

`api_level()` não usa `activity_`: basta `env_` e a classe
`Build$VERSION`. Ele pega o `jfieldID` com `GetStaticFieldID` e lê via
`GetStaticIntField`. Se `env_`, a classe ou o campo estiverem ausentes,
devolve `0`.

> **Observação:** a leitura de string usa `GetStringUTFChars` com
> `ReleaseStringUTFChars` e libera `value`/`build` ao final. O retorno é um
> `std::string` copiado — não é necessário liberar nada no chamador.

No `device-demo`:

```cpp
LOGI("device=%s android=%s api=%d bateria=%d%%",
     Device::instance(nullptr, nullptr).device_model().c_str(),
     Device::instance(nullptr, nullptr).android_version().c_str(),
     Device::instance(nullptr, nullptr).api_level(),
     Device::instance(nullptr, nullptr).battery_level());
```

---

## 7. Brilho: `set_brightness()`

```cpp
/// Ajusta brilho 0..1 (-1 = automático).
void set_brightness(float value) {
    if (!env_ || !activity_) return;
    jclass win_clazz = find_class("android/view/Window");
    jmethodID get_window = env_->GetMethodID(
        find_class("android/app/Activity"), "getWindow", "()Landroid/view/Window;");
    jobject window = env_->CallObjectMethod(activity_, get_window);
    if (!window) return;
    jclass params_clazz = find_class("android/view/WindowManager$LayoutParams");
    jmethodID get_attrs = env_->GetMethodID(
        win_clazz, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;");
    jobject attrs = env_->CallObjectMethod(window, get_attrs);
    if (attrs && params_clazz) {
        jfieldID screen = env_->GetFieldID(params_clazz, "screenBrightness", "F");
        const float clamped = value < 0 ? -1.0f
            : (value > 1.0f ? 1.0f : value);
        if (screen) env_->SetFloatField(attrs, screen, clamped);
        jmethodID set_attrs = env_->GetMethodID(
            win_clazz, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V");
        if (set_attrs) env_->CallVoidMethod(window, set_attrs, attrs);
        env_->DeleteLocalRef(attrs);
    }
    env_->DeleteLocalRef(params_clazz);
    env_->DeleteLocalRef(window);
    env_->DeleteLocalRef(win_clazz);
}
```

### 7.1 O clamp

| Entrada | Valor escrito em `screenBrightness` |
|---|---|
| `value < 0` (qualquer negativo) | `-1.0f` → **brilho automático** |
| `0 <= value <= 1` | o próprio `value` |
| `value > 1` | `1.0f` |

Não existe campo `brightness_mode` nem lock de timeout: o código apenas
escreve o float em `WindowManager.LayoutParams.screenBrightness` e chama
`Window.setAttributes()`. O `-1` é o valor que o Android interpreta como
"usar o brilho automático do sistema".

### 7.2 Caminho JNI

| Passo | Método/campo |
|---|---|
| 1 | `Activity.getWindow()` → `Window` |
| 2 | `Window.getAttributes()` → `WindowManager$LayoutParams` |
| 3 | `LayoutParams.screenBrightness` (`F`) via `GetFieldID`/`SetFloatField` |
| 4 | `Window.setAttributes(LayoutParams)` via `GetMethodID`/`CallVoidMethod` |

`window` nulo → retorno imediato. `attrs` ou `params_clazz` nulos → pula a
escrita. `screen`/`set_attrs` nulos → pula apenas a operação correspondente.
Ver §9 sobre os pontos **não** protegidos.

---

## 8. Tabela completa de métodos

### Públicos

| Assinatura real | Categoria | Retorno / efeito | Observações |
|---|---|---|---|
| `static Device& instance(JNIEnv* env, jobject activity)` | Singleton | Devolve o singleton; cria na primeira chamada | Argumentos não nulos **atualizam** `env_`/`activity_`; `nullptr`s apenas reaproveitam |
| `void vibrate(int millis)` | Vibração | Chama `Vibrator.vibrate(long)` | Requer `android.permission.VIBRATE`; falhas silenciosas |
| `int battery_level() const` | Bateria | `0..100`, ou `-1` se indisponível | `level * 100 / scale` (inteiro); usa sticky `BATTERY_CHANGED` |
| `bool is_charging() const` | Bateria | `plugged != 0` | `false` também significa "indisponível" |
| `Orientation orientation() const` | Tela | `Portrait` / `Landscape` / `Unknown` | `Display.getRotation()`: 0/2 → Portrait, 1/3 → Landscape |
| `std::string device_model() const` | Identidade | `Build.MODEL` | `""` em falha |
| `std::string android_version() const` | Identidade | `Build$VERSION.RELEASE` | `""` em falha |
| `int api_level() const` | Identidade | `Build$VERSION.SDK_INT` | `0` em falha |
| `void set_brightness(float value)` | Tela | Escreve `screenBrightness` | Clamp: `<0 → -1`, `>1 → 1`; `-1` = automático |

### Privados (contexto, não são API)

| Assinatura | Papel |
|---|---|
| `explicit Device(JNIEnv* env, jobject activity)` | Guarda `env_`/`activity_` |
| `jclass find_class(const char* name) const` | `env_ ? env_->FindClass(name) : nullptr` |
| `jobject system_service(const char* name) const` | `Context.getSystemService(String)`; checa classe e método |
| `jobject register_receiver(const char* action) const` | `registerReceiver(nullptr, IntentFilter(action))` → sticky Intent |
| `std::string build_string_field(const char*, const char*) const` | Lê campo `static String` e devolve `std::string` |

Campos de estado: `JNIEnv* env_ = nullptr;` e `jobject activity_ = nullptr;`.
Não existem, no header: `~Device()`, `shutdown()`, cache de bateria,
`unregisterReceiver()`, callback de bateria, controle de vibrador,
`screen_brightness()` (getter) ou `is_portrait()`.

---

## 9. Como a API lida com `jclass`/`jmethodID` nulos

O padrão geral do header é **checar e degradar**: se a classe ou o ID do
método não pôde ser obtido, o método retorna um valor "indisponível" ou não
faz nada. Não há exceções C++ nem logs — tudo é silencioso.

| Método | `jclass` nulo | `jmethodID` nulo | Valor devolvido |
|---|---|---|---|
| `vibrate` | `if (clazz)` protege toda a chamada | `if (vibrate)` protege o `CallVoidMethod` | `void` (não vibra) |
| `battery_level` | **Não há checagem** de `clazz`/`get_int` | **Não há checagem** | `-1` apenas se faltar env/activity ou Intent |
| `is_charging` | **Não há checagem** de `clazz`/`get_int` | **Não há checagem** | `false` apenas se faltar env/activity ou Intent |
| `orientation` | `context_class` → `Unknown`; `wm_class`/`display_class` via ternário | `get_wm` → `Unknown`; `get_display`/`get_rotation` via ternário | `Unknown` |
| `device_model` / `android_version` | `if (!build) return {}` | `if (!field) ... return {}` | `""` |
| `api_level` | `if (!build) return 0` | `field ? GetStaticIntField : 0` | `0` |
| `set_brightness` | `params_clazz` checado no bloco; **`win_clazz` e a classe `Activity` não são checados** | `screen`/`set_attrs` checados; **`get_window`/`get_attrs` não são checados** | `void` |
| `system_service` | `if (!context_class) return nullptr` | `if (!get_service) return nullptr` | `nullptr` |
| `register_receiver` | Checa `intent_filter` e `ctx` | Checa `ctor` e `register_method` | `nullptr` |

Detalhes que valem destaque:

- `register_receiver` é o mais cuidadoso: se `ctor`, `ctx` ou
  `register_method` falham, ele libera o `intent_filter` antes de devolver
  `nullptr`.
- `build_string_field` libera `build` tanto no sucesso quanto na falha de
  `field`, e só chama `ReleaseStringUTFChars` se `GetStringUTFChars` tiver
  devolvido ponteiro não nulo.
- `DeleteLocalRef` é chamado em vários pontos com variáveis que podem estar
  nulas (por exemplo `params_clazz`, `win_clazz`); isso é aceitável pela
  especificação JNI, que trata referência nula como no-op.
- **Assimetria real:** `battery_level()`/`is_charging()` e parte de
  `set_brightness()` não blindam `clazz`/`jmethodID`. Se `FindClass` falhar
  nessas rotas, o código segue em frente com um ID nulo. Na prática as classes
  usadas (`android/content/Intent`, `android/view/Window`,
  `android/app/Activity`) fazem parte do framework Android e normalmente são
  encontradas, mas o código não trata a falha.

---

## 10. Exemplo completo

Baseado em `examples/device-demo/jni/main.cpp`, com foco em `Device`. Ele
inicializa o singleton com o `JNIEnv` da thread nativa, imprime a identidade do
aparelho, vibra ao tocar, registra bateria/orientação de tempos em tempos e
alterna brilho automático/manual com a tecla de volume para cima.

```cpp
// jni/main.cpp — utilidades do dispositivo
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#include <vector>

#define LOG_TAG "device-guide"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class DeviceDemo : public App {
public:
    explicit DeviceDemo(android_app* app) : App(app) {}

private:
    void on_ready() override {
        // 1) Device NÃO é inicializado pelo App. Obtenha o JNIEnv e passe
        //    JNIEnv + Activity pelo menos uma vez.
        JNIEnv* env = nullptr;
        activity()->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (!env) activity()->vm->AttachCurrentThread(&env, nullptr);
        Device::instance(env, activity()->clazz);

        Device& dev = Device::instance(nullptr, nullptr);  // reaproveita
        LOGI("device=%s android=%s api=%d",
             dev.device_model().c_str(),
             dev.android_version().c_str(),
             dev.api_level());
        LOGI("bateria=%d%% charging=%d",
             dev.battery_level(), dev.is_charging() ? 1 : 0);

        switch (dev.orientation()) {
            case Device::Orientation::Portrait:  LOGI("orient=Portrait");  break;
            case Device::Orientation::Landscape: LOGI("orient=Landscape"); break;
            case Device::Orientation::Unknown:   LOGI("orient=Unknown");   break;
        }

        // Requer <uses-permission android:name="android.permission.VIBRATE" />
        dev.vibrate(80);

        set_scene(&scene_);
        scene_.set_clear_color(0.05f, 0.05f, 0.10f);
    }

    void on_surface_created() override { scene_.init(window()); }
    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_update(float) override {
        if (frames() % 300 != 0) return;  // não consulte a bateria todo frame
        const Device& dev = Device::instance(nullptr, nullptr);
        LOGI("bateria=%d%% charging=%d orient=%d",
             dev.battery_level(),
             dev.is_charging() ? 1 : 0,
             static_cast<int>(dev.orientation()));
    }

    void on_draw() override {
        // Apenas desenho; nenhuma chamada JNI por frame.
        scene_.draw(tile_, material_, model_.data());
    }

    void on_touch(TouchAction action,
                  const std::vector<TouchPoint>& pts) override {
        if (action == TouchAction::Down && !pts.empty()) {
            Device::instance(nullptr, nullptr).vibrate(40);
        }
    }

    void on_key(int32_t code, bool down) override {
        if (!down) return;
        if (code == 4) request_exit();                     // BACK
        if (code == 24) {                                  // VOLUME_UP
            brightness_auto_ = !brightness_auto_;
            Device::instance(nullptr, nullptr).set_brightness(
                brightness_auto_ ? -1.0f : 0.35f);
            LOGI("brilho: %s", brightness_auto_ ? "automatico" : "0.35");
        }
    }

    GLES2Scene scene_;
    MeshData tile_ = MeshData::quad();
    Material material_;
    Mat4 model_ = mat4_scale(160.0f, 160.0f, 1.0f);
    bool brightness_auto_ = true;
};

APP_MAIN(DeviceDemo)
```

Manifesto mínimo (o do `device-demo` é exatamente nesse espírito):

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.apkfast.devicedemo">

    <uses-feature android:glEsVersion="0x00020000" android:required="true" />
    <uses-permission android:name="android.permission.VIBRATE" />

    <application android:label="device demo" android:hasCode="false">
        <activity android:name="android.app.NativeActivity"
                  android:exported="true"
                  android:configChanges="orientation|keyboardHidden|screenSize">
            <meta-data android:name="android.app.lib_name" android:value="main" />
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
```

---

## 11. Armadilhas e diagnóstico

### 11.1 Permissão ausente no manifesto

`vibrate()` chama `Vibrator.vibrate(long)`, mas sem
`<uses-permission android:name="android.permission.VIBRATE" />` o pacote não
tem a permissão registrada: a chamada simplesmente não produz efeito e o
helper não loga nem retorna erro. O manifesto do `device-demo` declara a tag;
projetos criados com o assistente também a têm via `COMMON_PERMISSIONS`.
Se você escreveu o manifesto à mão, confira.

### 11.2 `JNIEnv` da thread correta

`Device` não obtém o env sozinho. O `App` inicializa `TextInput` com o env da
UI thread e injeta o `AAssetManager`, mas **não** chama `Device::instance()`.
Se você esquecer a inicialização, `env_` continua nulo e todos os métodos
devolvem seus valores de falha (`-1`, `false`, `""`, `0`, `Unknown`, nenhuma
vibração/brilho).

Se a primeira chamada for `Device::instance(nullptr, nullptr)`, o singleton é
criado vazio; uma chamada posterior com `env`/`activity` válidos corrige as
refs (§2.1). Use `vm->GetEnv(JNI_VERSION_1_6)` com
`vm->AttachCurrentThread()` como fallback, como no exemplo, e guarde o
`JNIEnv*` usado na mesma thread que fará as chamadas.

### 11.3 Referências locais

Cada método cria referências locais (`jclass`, `jstring`, `jobject`) e as
libera com `DeleteLocalRef` antes de retornar — inclusive em caminhos de
falha nas rotinas mais cuidadosas (`register_receiver`, `build_string_field`).
Não tente guardar esses `jclass`/`jmethodID` fora da chamada: o header não
expõe cache nenhum. O único estado persistente são `env_` e `activity_`.

> **Observação:** `activity_` é o `jobject` recebido, sem `NewGlobalRef`. Se
> a Activity for recriada, o `App` não avisa `Device`; para atualizar, chame
> `Device::instance(env, nova_activity)` com argumentos não nulos.

### 11.4 Brilho: `-1` é automático (e qualquer negativo vira `-1`)

`set_brightness(-1.0f)` pede brilho automático ao sistema. Como o clamp é
`value < 0 ? -1.0f : ...`, passar `-0.2f` ou `-10.0f` tem o **mesmo** efeito.
Passar `2.0f` resulta em `1.0f`. Não existe getter: depois de mudar, o código
não oferece como consultar o valor atual — cabe a você guardar o último valor
enviado (como `brightness_auto_` no exemplo).

### 11.5 `registerReceiver` com receiver nulo

`battery_level()` e `is_charging()` chamam
`Context.registerReceiver(nullptr, IntentFilter(BATTERY_CHANGED))`. Isso é
intencional para receber o Intent *sticky* mais recente sem registrar um
receiver permanente — e explica por que não há `unregisterReceiver()`. Cada
chamada faz trabalho JNI novo, então evite chamá-las a cada frame; use um
contador de frames ou um evento, como no exemplo.

### 11.6 `jclass`/`jmethodID` nulos: degrade, mas nem sempre com segurança

A maioria dos métodos checa e degrada (tabela completa em §9). Porém
`battery_level()`/`is_charging()` não checam `GetObjectClass`/`GetMethodID`, e
`set_brightness()` passa o resultado de `find_class("android/app/Activity")`
direto para `GetMethodID` e usa `win_clazz`/`get_window`/`get_attrs` sem
verificação prévia. Se alguma dessas classes não for encontrada, o comportamento
não é tratado pelo helper. As classes envolvidas são do framework Android, mas
o código não oferece proteção adicional.

### 11.7 `Device::instance(nullptr, nullptr)` só funciona depois da 1ª chamada válida

O `device-demo` usa esse atalho em `on_ready()`, `on_tick()` e `on_touch()`.
Ele funciona porque o singleton já foi criado com `env`/`activity` no início
de `on_ready()`. Se você copiar só as linhas do `LOGI` sem a inicialização,
todos os valores virão vazios/negativos/zero.

### 11.8 Diagnóstico rápido

| Sintoma | Causa provável no código |
|---|---|
| `vibrate()` não vibra | Falta `VIBRATE` no manifesto, ou `Device` nunca inicializado com env/activity |
| `battery_level() == -1` sempre | `env_`/`activity_` nulos, ou `registerReceiver` devolveu `nullptr` |
| `is_charging() == false` na tomada | `plugged` veio `0`; o código não distingue tipos de carregamento |
| `orientation() == Unknown` | Falha em `Context`/`WindowManager`/`Display` (ou Device não inicializado) |
| `device_model()`/`android_version()` vazias, `api_level() == 0` | `FindClass`/`GetStaticFieldID` falharam ou `env_` nulo |
| Brilho não muda | `window`/`attrs` nulos, ou valor passado já era o automático; não há log de erro |
| Valores "zero" logo após criar o singleton | Primeira chamada foi com `nullptr, nullptr`; reinicialize com env/activity válidos |

---

## 12. Resumo rápido

```text
Chamador (ex.: App::on_ready)
  └─ JNIEnv* env = GetEnv(...) ou AttachCurrentThread(...)
     Device::instance(env, activity()->clazz)   ← obrigatório

 Depois disso: Device::instance(nullptr, nullptr) reaproveita o singleton

 vibrate(ms)          → Vibrator.vibrate(long)       [precisa de VIBRATE]
 battery_level()      → sticky BATTERY_CHANGED       [0..100 ou -1]
 is_charging()        → extra "plugged" != 0
 orientation()        → Display.getRotation()        [0/2=Portrait, 1/3=Landscape]
 device_model()       → Build.MODEL                  ["" se falhar]
 android_version()    → Build$VERSION.RELEASE        ["" se falhar]
 api_level()          → Build$VERSION.SDK_INT        [0 se falhar]
 set_brightness(v)    → clamp: <0→-1 (auto), >1→1; Window.setAttributes()
```
