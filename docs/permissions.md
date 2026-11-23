# Sistema de permissões em runtime do apkfast

> Guia de referência completo, escrito a partir do código que existe no
> repositório: `include/nativeapp/permission_types.hpp`,
> `include/nativeapp/permissions.hpp`, `include/nativeapp/permissions_jni.hpp`,
> integração em `include/nativeapp/app.hpp` e os exemplos
> `examples/permissions-test`, `examples/raylib-permissions` e
> `examples/sdl3-permissions`.

---

## 1. Visão geral

O apkfast oferece **duas APIs paralelas** para permissões de runtime no Android,
ambas no namespace `nativeapp`:

| API | Header | Entrada | Engine típico |
|---|---|---|---|
| `Permissions` | `nativeapp/permissions.hpp` | `ANativeActivity*` | `nativeapp::App`, raylib |
| `JniPermissions` | `nativeapp/permissions_jni.hpp` | `JNIEnv*` + `jobject` (Activity) | SDL2, SDL3 |

Ambas usam **JNI direto** (sem código Java próprio, sem `onActivityResult`, sem
`ActivityCompat`): chamam `checkSelfPermission`,
`shouldShowRequestPermissionRationale` e `requestPermissions` na própria
Activity, e **`requestPermissions` com o request code**
`kRequestCode = 0x4e415050` (`'NAPP'`).

```cpp
#include <nativeapp/nativeapp.hpp>     // umbrella: inclui permissions.hpp e permissions_jni.hpp
// ou, de forma explícita:
// #include <nativeapp/permissions.hpp>
// #include <nativeapp/permissions_jni.hpp>
```

O modelo de uso recomendado é **assíncrono**:

```cpp
auto& perms = nativeapp::Permissions::instance(activity());
perms.request_async(
    {"android.permission.CAMERA"},
    [](const nativeapp::PermissionResult& r) {
        if (r.all_granted) { /* usa a câmera */ }
        else if (r.permanently_denied) { /* manda para Configurações */ }
    });
```

O resultado real do diálogo do sistema **nunca** está no retorno de `request()`.
Ele chega depois, via `update()` (chamado automaticamente pelo `App` a cada
frame, ou manualmente no loop de raylib/SDL).

---

## 2. Tipos compartilhados (`permission_types.hpp`)

### 2.1 `PermissionState`

```cpp
enum class PermissionState {
    Unknown,
    Granted,
    Denied,
    PermanentlyDenied
};
```

| Valor | Significado no código |
|---|---|
| `Unknown` | Retornado por `state_of()` quando não há `JNIEnv` disponível (`Permissions`) ou quando `env_`/`activity_` são nulos (`JniPermissions`). |
| `Granted` | `checkSelfPermission(...) == 0`; também é o estado devolvido em API < 23. |
| `Denied` | Negada, mas ainda pode ser solicitada. Inclui o caso "nunca pedida" e o caso em que `shouldShowRequestPermissionRationale` é `true`. |
| `PermanentlyDenied` | Já foi negada nesta sessão **e** `shouldShowRequestPermissionRationale` agora é `false` ("não perguntar novamente"). |

### 2.2 `PermissionResult`

```cpp
struct PermissionResult {
    bool all_granted = false;
    std::vector<std::string> granted;
    std::vector<std::string> denied;
    bool should_show_rationale = false;
    bool permanently_denied = false;

    bool empty() const { return granted.empty() && denied.empty(); }
};
```

| Campo | Tipo | Como é preenchido |
|---|---|---|
| `all_granted` | `bool` | `true` em `request()` quando a lista está vazia, API < 23, ou tudo já estava concedido; em `update()` vale `denied.empty()`. |
| `granted` | `std::vector<std::string>` | Nomes concedidos. Em `request()` com API < 23 ou "tudo já concedido", recebe **todos** os nomes pedidos. |
| `denied` | `std::vector<std::string>` | Em `request()`, recebe **a lista filtrada** de permissões ainda não concedidas (`pending`). Em `update()`, recebe as que não estão `Granted`. |
| `should_show_rationale` | `bool` | `true` em `update()` se alguma permissão ficou no estado `Denied` **após ter sido pedida** (rationale disponível). |
| `permanently_denied` | `bool` | `true` em `update()` se alguma permissão ficou no estado `PermanentlyDenied`. |
| `empty()` | método | `true` quando `granted` **e** `denied` estão vazios. |

> Atenção: `should_show_rationale` e `permanently_denied` são preenchidos pelo
> `update()`, não pelo `request()`. O `PermissionResult` devolvido por
> `request()` só tem `all_granted`/`granted`/`denied`.

---

## 3. API A — `Permissions` (`ANativeActivity*`)

### 3.1 Todas as assinaturas públicas

Definidas em `include/nativeapp/permissions.hpp`:

| Assinatura | Finalidade |
|---|---|
| `using Callback = std::function<void(const PermissionResult&)>;` | Tipo do callback de `request_async()`. |
| `static Permissions& instance(ANativeActivity* activity)` | Singleton. Cria na primeira chamada; se `activity` não for nulo e for diferente do guardado, chama `reattach()`. |
| `void reattach(ANativeActivity* activity)` | Troca a Activity após recriação: libera e recacheia a global ref da classe, limpa `pending_`, `callback_`, `response_received_` e `snapshot_`. |
| `bool runtime_permissions_available() const` | Lê `Build$VERSION.SDK_INT` e retorna `sdk >= 23`; retorna `false` se não houver `JNIEnv`. |
| `PermissionState state_of(std::string_view permission) const` | Estado detalhado (ver § 6). |
| `bool is_granted(std::string_view permission) const` | Atalho para `state_of(permission) == PermissionState::Granted`. |
| `PermissionResult request(const std::vector<std::string>& permissions)` | Dispara `requestPermissions` para as ainda não concedidas e devolve um resultado **provisório**. |
| `void request_async(const std::vector<std::string>& permissions, Callback callback)` | `request()` + registro de callback + `awaiting_response_ = true`. |
| `void update()` | Drena o resultado: detecta mudança/resposta, monta o `PermissionResult` final e invoca o callback uma vez. |
| `void notify_activity_result()` | Marca `response_received_ = true` (chamado pelo `App` no `APP_CMD_RESUME`). |
| `bool has_pending() const` | `!pending_.empty() && bool(callback_)`. |
| `void open_app_settings() const` | Alias de `openAppSettings()`. |
| `void openAppSettings() const` | Abre `android.settings.APPLICATION_DETAILS_SETTINGS` com `Uri.parse("package:" + package_name())`. |
| `std::string package_name() const` | `getPackageName()` via JNI (com `GetStringUTFChars`/`ReleaseStringUTFChars`). |
| `static constexpr int kRequestCode = 0x4e415050;` | Request code usado em `requestPermissions` (`'NAPP'`). |
| `void shutdown()` | Libera refs globais, limpa callback/pending, zera `activity_class_` e `activity_`. Chamado por `App::finish()`. |

Membros privados relevantes para entender o comportamento: `activity_`,
`activity_class_` (global ref da classe da Activity), `pending_`, `callback_`,
`response_received_`, `awaiting_response_`, `snapshot_`, `requested_` e
`denied_before_` (mutable). O `env()` privado obtém o `JNIEnv` via
`activity_->vm->GetEnv`/`AttachCurrentThread` com `JNI_VERSION_1_6`.

### 3.2 Fluxo interno de `request()`

1. Lista vazia → `all_granted = true`, retorno imediato.
2. Sem `JNIEnv` → `denied = permissions`.
3. API < 23 → `all_granted = true`, `granted = permissions`.
4. Filtra `pending` = permissões com `!is_granted(p)`; se vazio →
   `all_granted = true`, `granted = permissions`.
5. Monta `jobjectArray` de `String`, chama
   `requestPermissions(array, kRequestCode)`.
6. **Marca `requested_` antes de consultar o rationale.**
7. Salva `snapshot_[permissão] = state_of(permissão)` para detectar mudanças.
8. Devolve `result.denied = pending` — este é o resultado *provisório*.

### 3.3 `update()` — o "pump" do resultado

```cpp
bool changed = response_received_ && awaiting_response_;
// para cada permissão pendente:
//   state = state_of(p)
//   se state != snapshot_[p]  -> changed = true
//   snapshot_[p] = state
//   classifica em granted/denied e ajusta should_show_rationale / permanently_denied
if (!changed) return;
result.all_granted = result.denied.empty();
response_received_ = false;
awaiting_response_ = false;
// move callback_ para local, limpa callback_/pending_/snapshot_ e chama callback(result)
```

O callback é **one-shot**: após disparar, `callback_` vira `nullptr` e `pending_`
é esvaziado. `has_pending()` passa a `false`.

---

## 4. API B — `JniPermissions` (`JNIEnv*` + `jobject`)

### 4.1 Todas as assinaturas públicas

Definidas em `include/nativeapp/permissions_jni.hpp`:

| Assinatura | Finalidade |
|---|---|
| `using Callback = std::function<void(const PermissionResult&)>;` | Tipo do callback. |
| `static constexpr int kRequestCode = 0x4e415050;` | Mesmo request code da API A. |
| `static JniPermissions& instance(JNIEnv* env, jobject activity)` | Singleton; reatachá se `activity` não nulo e diferente do atual. |
| `void reattach(JNIEnv* env, jobject activity)` | Libera a global ref antiga, guarda `env_`/`activity_`, recacheia a classe, limpa `pending_`, `callback_`, `snapshot_`, `awaiting_response_` e `response_received_`. |
| `bool runtime_permissions_available() const` | `Build$VERSION.SDK_INT >= 23`; `false` sem `env_`. |
| `PermissionState state_of(std::string_view permission) const` | Mesma lógica da API A, usando `env_` + `activity_`. |
| `bool is_granted(std::string_view permission) const` | Atalho para `Granted`. |
| `PermissionResult request(const std::vector<std::string>& permissions)` | Mesmo fluxo da API A. |
| `void request_async(const std::vector<std::string>& permissions, Callback callback)` | Registra callback e aguarda `update()`. |
| `void notify_activity_result()` | `response_received_ = true`. |
| `void update()` | Mesmo pump da API A; retorna cedo também se `!env_`. |
| `bool has_pending() const` | `!pending_.empty() && bool(callback_)`. |
| `void openAppSettings() const` | Abre Configurações do app via Intent explícito. |
| `std::string package_name() const` | `getPackageName()`. |
| `void set_env(JNIEnv* env)` | Troca somente o `env_` guardado. |
| `JNIEnv* env() const` | Getter do `env_`. |
| `jobject activity() const` | Getter do `activity_`. |
| `void shutdown()` | Com `env_` e classe válidos, chama `UnregisterNatives` + `DeleteGlobalRef`; limpa `pending_`, `callback_` e `activity_`. |

### 4.2 Diferenças em relação a `Permissions`

| Aspecto | `Permissions` | `JniPermissions` |
|---|---|---|
| Obtenção do JNI | Automática via `ANativeActivity::vm` (`GetEnv`/`AttachCurrentThread`) | `JNIEnv*` é fornecido e guardado; há `set_env()`/`env()` |
| Getter da Activity | não exposto (usa `activity_->clazz`) | `activity()` |
| Abrir Configurações | `openAppSettings()` **e** `open_app_settings()` | apenas `openAppSettings()` |
| `reattach()` | Não redefine `awaiting_response_` explicitamente | Redefine `awaiting_response_` e `response_received_` |
| `shutdown()` | `DeleteGlobalRef` da classe; zera `activity_class_` e `activity_` | `UnregisterNatives` + `DeleteGlobalRef`; zera `activity_` |
| Uso típico | `nativeapp::App`, raylib | SDL2, SDL3 |

> `App::finish()` chama **`Permissions::instance(nullptr).shutdown()`** e
> **`JniPermissions::instance(nullptr, nullptr).shutdown()`**. Como `instance()`
> só reatacha quando recebe um `activity` não nulo, esses argumentos nulos apenas
> alcançam o singleton já existente para desligá-lo.

---

## 5. `request()` "síncrono" vs `request_async()`

| | `request(permissions)` | `request_async(permissions, callback)` |
|---|---|---|
| Retorno | `PermissionResult` imediato e **provisório** | `void` |
| Dispara diálogo | Sim, via `requestPermissions` | Sim — chama `request()` internamente |
| Callback | Nunca | Imediato se `initial.all_granted`; caso contrário, no `update()` |
| Estado interno | Grava `requested_` e `snapshot_`; **não** guarda `pending_`/`callback_` | Guarda `pending_ = permissions` (lista completa passada), `callback_`, `awaiting_response_ = true`, `response_received_ = false` |
| Para que serve | Iniciar a solicitação e inspecionar a lista pendente | Fluxo recomendado: reagir ao resultado real |

```cpp
// Síncrono: o retorno NÃO é a resposta do usuário
auto provisional = perms.request({"android.permission.CAMERA"});
LOGI("pendentes=%zu", provisional.denied.size());
// ... depois, em algum ponto, você mesmo teria que consultar state_of()/update()

// Assíncrono (recomendado nos exemplos)
perms.request_async({"android.permission.CAMERA"},
    [](const PermissionResult& r) { /* resultado real */ });
```

Mesmo no caminho "síncrono", o cabeçalho deixa claro: *"O resultado real é
assíncrono; use `update()` ou `request_async()` para receber a resposta."*

---

## 6. `PermanentlyDenied` e como ele é detectado

A detecção é feita em `state_of()`, com três peças:

| Peça | Papel |
|---|---|
| `requested_` (`std::unordered_set`) | Marca as permissões passadas por `request()` — inseridas **antes** de consultar o rationale. |
| `shouldShowRequestPermissionRationale` | Consultado via Activity somente se a permissão já está em `requested_`. |
| `denied_before_` (`mutable std::unordered_set`) | Guarda as permissões que, após terem sido pedidas, ainda retornaram `rationale == true`. |

Sequência exata do código:

```text
checkSelfPermission == 0                 -> Granted
!was_requested(permission)               -> Denied        (nunca pedida)
rationale == true                        -> denied_before_.insert(p); Denied
rationale == false && denied_before_ tem -> PermanentlyDenied
rationale == false && denied_before_ não tem -> Denied
```

| Situação | `requested_` | `rationale` | `denied_before_` | Estado |
|---|---|---|---|---|
| Primeira consulta, nunca pedida | não | (não consultado) | — | `Denied` |
| Diálogo aberto e negado normalmente | sim | `true` | passa a conter | `Denied` (com `should_show_rationale = true`) |
| Negado com "não perguntar novamente" | sim | `false` | já contém | `PermanentlyDenied` |
| API < 23 | — | (não consultado) | — | `Granted` |

O comentário do header registra a intenção: *"rationale=false SÓ é permanente se
já houve uma negação registrada nesta sessão (caso contrário é a primeira
negação do sistema)"*. `requested_` e `denied_before_` são conjuntos **em memória
no singleton** — não há persistência em arquivo; um novo processo/singleton
começa com ambos vazios.

No `update()`, o estado vira sinal no resultado:

```cpp
if (state == PermissionState::Granted)        result.granted.push_back(p);
else {
    result.denied.push_back(p);
    if (state == PermissionState::Denied)             result.should_show_rationale = true;
    else if (state == PermissionState::PermanentlyDenied) result.permanently_denied = true;
}
```

---

## 7. O gatilho do callback: `APP_CMD_RESUME` → `notify_activity_result()` → `update()`

O diálogo de permissões **pausa a Activity**. O `App` fecha o ciclo em três pontos:

| Momento | Código em `app.hpp` | Efeito |
|---|---|---|
| Cada frame (antes de `on_update`/`on_draw`) | `on_tick()` → `Permissions::instance(app_->activity).update()` | Verifica mudanças e dispara o callback |
| `APP_CMD_RESUME` | `paused_ = false`; reinicia `last_frame_`; `Permissions::instance(app_->activity).notify_activity_result()`; `on_resume()`; volta para `Ready`; `activate_surface()` | Sinaliza que a Activity voltou do diálogo — **mesmo que o usuário tenha negado** |
| `finish()` | `Permissions::instance(nullptr).shutdown()` + `JniPermissions::instance(nullptr, nullptr).shutdown()` | Libera refs globais e limpa callbacks antes de `on_destroy()` |

Trecho real do tratamento de comando:

```cpp
case APP_CMD_RESUME:
    paused_ = false;
    last_frame_ = std::chrono::steady_clock::now();
    Permissions::instance(app_->activity).notify_activity_result();
    on_resume();
    if (state_ == Lifecycle::Paused) state_ = Lifecycle::Ready;
    activate_surface();   // a janela pode ser nova
    break;
```

Por que `notify_activity_result()` é necessário? Porque o usuário pode **negar
sem mudar o estado** (`Denied` antes e depois). Nesse caso a comparação com
`snapshot_` não acusaria mudança. O flag resolve isso:

```cpp
bool changed = response_received_ && awaiting_response_;
```

`request_async()` ainda zera `response_received_ = false` de propósito: *"Descarta
o RESUME inicial (que ocorreu antes de pedir); só o RESUME posterior ao diálogo
deve disparar o callback."*

**Fora do `nativeapp::App`** (raylib/SDL), ninguém chama
`notify_activity_result()` automaticamente. Os exemplos `raylib-permissions` e
`sdl3-permissions` apenas chamam `perms.update()` a cada iteração do loop; neles,
o disparo acontece pela diferença de estado em relação ao `snapshot_` (ou você
pode chamar `notify_activity_result()` explicitamente no seu código).

---

## 8. Matriz por engine (getters reais)

| Engine | Como obter a Activity | API de permissão |
|---|---|---|
| `nativeapp::App` | `activity()` → `ANativeActivity*` (também `native_app()->activity`) | `Permissions::instance(activity())` — chamado em `on_ready()`, enquanto `App::on_tick()` faz o `update()` |
| raylib | `extern "C" android_app* GetAndroidApp(void);` → `GetAndroidApp()->activity` | `Permissions::instance(GetAndroidApp()->activity)` + `perms.update()` no loop |
| SDL2 | `SDL_AndroidGetJNIEnv()` → `JNIEnv*`; `SDL_AndroidGetActivity()` → `jobject` | `JniPermissions::instance(env, SDL_AndroidGetActivity())` + `update()` no loop |
| SDL3 | `SDL_GetAndroidJNIEnv()` → `JNIEnv*`; `SDL_GetAndroidActivity()` → `jobject` | `JniPermissions::instance(SDL_GetAndroidJNIEnv(), SDL_GetAndroidActivity())` + `update()` no loop |

SDL2:

```cpp
#include <SDL.h>
#include <SDL_system.h>
#include <nativeapp/permissions_jni.hpp>

JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
auto& perms = nativeapp::JniPermissions::instance(env, activity);
```

SDL3 (padrão de `examples/sdl3-permissions/jni/main.cpp`):

```cpp
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_main.h>
#include <nativeapp/permissions_jni.hpp>

JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
auto& perms = nativeapp::JniPermissions::instance(env, activity);
```

SDL3 também expõe aliases de compatibilidade (`SDL_AndroidGetActivity` e
`SDL_AndroidGetJNIEnv`) em `SDL3/SDL_oldnames.h`; os nomes nativos da tabela são
os preferidos.

Exemplos no repositório:

| Exemplo | API usada | Permissões pedidas |
|---|---|---|
| `examples/permissions-test` | `Permissions::instance(activity())` | `CAMERA`, `RECORD_AUDIO`, `ACCESS_FINE_LOCATION` |
| `examples/raylib-permissions` | `Permissions::instance(GetAndroidApp()->activity)` | `CAMERA`, `RECORD_AUDIO` |
| `examples/sdl3-permissions` | `JniPermissions::instance(env, activity)` | `CAMERA`, `RECORD_AUDIO` |

---

## 9. Permissões declaradas no `AndroidManifest.xml`

As permissões pedidas em C++ **também precisam constar no manifesto**. Os
exemplos declaram exatamente o que pedem:

`examples/permissions-test/AndroidManifest.xml`:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.apkfast.permstest">

    <uses-feature android:glEsVersion="0x00020000" android:required="true" />
    <uses-permission android:name="android.permission.CAMERA" />
    <uses-permission android:name="android.permission.RECORD_AUDIO" />
    <uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" />

    <application android:label="perm test" android:hasCode="false">
        <activity android:name="android.app.NativeActivity" ... />
    </application>
</manifest>
```

`examples/raylib-permissions/AndroidManifest.xml`: declara `CAMERA` e
`RECORD_AUDIO`, usa `android.app.NativeActivity` com
`android:hasCode="false"` e `android.app.lib_name = main`.

`examples/sdl3-permissions/AndroidManifest.xml`: declara `CAMERA` e
`RECORD_AUDIO`, mas a Activity é `com.apkfast.sdl3perms.MainActivity` (a classe
Java do SDL), com `android:configChanges="orientation|keyboardHidden|screenSize"`.

> **Armadilha:** esquecer o `<uses-permission>` correspondente é o erro mais
> comum. A chamada JNI existe, mas o sistema não tem a permissão registrada para
> o pacote — o resultado seguirá como negado (`denied`) e o diálogo não trará a
> concessão esperada. Mantenha a lista do `request_async()` idêntica às tags do
> manifesto.

---

## 10. API < 23: concedidas na instalação

`runtime_permissions_available()` lê `android.os.Build.VERSION.SDK_INT` e retorna
`sdk >= 23`. Quando é `false`:

| Método | Comportamento |
|---|---|
| `state_of()` | Retorna `PermissionState::Granted` **sem chamar** `checkSelfPermission` |
| `is_granted()` | `true` |
| `request()` | `all_granted = true`, `granted = permissions` (todas), nenhum diálogo, nenhuma `requestPermissions` |
| `request_async()` | Chama o callback **sincronamente** com esse resultado (`initial.all_granted == true`) |

Ou seja, em API < 23 o código de permissão é um no-op seguro: tudo já foi
concedido na instalação. Se não houver `JNIEnv`, a função retorna `false` (e
`state_of()` devolve `Unknown`), então trate `runtime_permissions_available()`
como parte do seu diagnóstico — o exemplo `permissions-test` faz exatamente isso:

```cpp
LOGI("runtime_permissions_available=%d", perms.runtime_permissions_available());
```

---

## 11. Armadilhas e detalhes operacionais

| # | Armadilha | O que acontece | Como evitar |
|---|---|---|---|
| 1 | Permissão ausente no manifesto | Não há concessão; resultado fica em `denied` | Declarar `<uses-permission>` para **cada** string passada a `request()`/`request_async()` |
| 2 | Callback não dispara sem `resume` | `response_received_` fica `false`, `changed` fica `false` | Use `App` (que chama `notify_activity_result()` no `APP_CMD_RESUME`) ou chame `notify_activity_result()` no seu próprio código; fora do `App`, mantenha `update()` no loop |
| 3 | Sobrescrever `on_tick()` sem chamar a base | `Permissions::update()` deixa de rodar e nenhum callback dispara | `void on_tick() override { App::on_tick(); /* ... */ }` |
| 4 | Activity recriada (rotação, configChanges) | A global ref da classe antiga vira inválida | `instance()` detecta ponteiro diferente e chama `reattach()`; no `App` isso é automático a cada frame. Com `JniPermissions`, chame `instance(env, novaActivity)`/`reattach()` novamente. Atenção: `reattach()` **descarta** `pending_` e `callback_` — um `request_async()` em voo é perdido |
| 5 | Guardar refs locais JNI | `jstring`, `jobjectArray`, `Intent`, `Uri` etc. são locais e inválidos após o retorno do método | As funções já chamam `DeleteLocalRef` em cada um; o singleton guarda só a **global ref** da classe da Activity. Não armazene esses locais entre frames |
| 6 | `shutdown()` manual no meio da execução | Zera `activity_`, libera `activity_class_` e descarta callback pendente | `App::finish()` já chama `shutdown()` para ambos os singletons **antes** de `on_destroy()`. Não precisa repetir; se precisar reutilizar depois, chame `instance(activity)` com uma Activity válida para recachear |
| 7 | Erro de JNI em `update()` sem `JNIEnv` | `Permissions::update()` retorna cedo; `JniPermissions` também exige `env_` | `JniPermissions::set_env(env)` pode atualizar o `env_` quando ele mudar de thread |
| 8 | Ler `PermissionResult` de `request()` como final | Os campos `should_show_rationale`/`permanently_denied` não são preenchidos lá | Use `request_async()` e leia o resultado no callback |

Também vale lembrar que `App::on_tick()` é virtual e é **o único** lugar que roda
o pump de permissões no `nativeapp::App`; e que `finish()` desliga os dois
singletons antes de `on_destroy()`, então `on_destroy()` não deve tentar usar a
API de permissões.

---

## 12. Exemplo completo (`nativeapp::App`)

Baseado em `examples/permissions-test/jni/main.cpp`, com tratamento explícito de
todos os campos do resultado:

```cpp
#include <nativeapp/nativeapp.hpp>   // inclui permissions.hpp e permissions_jni.hpp
#include <android/log.h>

#define LOG_TAG "perms-test"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

static const char* state_name(PermissionState state) {
    switch (state) {
        case PermissionState::Granted:           return "GRANTED";
        case PermissionState::Denied:            return "DENIED";
        case PermissionState::PermanentlyDenied: return "PERMANENTLY_DENIED";
        default:                                 return "UNKNOWN";
    }
}

class PermsApp : public App {
public:
    explicit PermsApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        auto& perms = Permissions::instance(activity());   // mesmo singleton do App

        LOGI("runtime_permissions_available=%d", perms.runtime_permissions_available());
        LOGI("package=%s", perms.package_name().c_str());

        for (const char* permission : permissions_) {
            LOGI("antes: %s = %s", permission, state_name(perms.state_of(permission)));
        }

        perms.request_async(
            {permissions_[0], permissions_[1], permissions_[2]},
            [](const PermissionResult& result) {
                LOGI("=== CALLBACK ===");
                LOGI("all_granted=%d rationale=%d permanent=%d",
                     result.all_granted, result.should_show_rationale,
                     result.permanently_denied);
                for (const auto& p : result.granted) LOGI("  GRANTED:   %s", p.c_str());
                for (const auto& p : result.denied)  LOGI("  DENIED:    %s", p.c_str());

                // PermanentlyDenied: o caminho é abrir as Configurações do app.
                // (o singleton tem openAppSettings()/open_app_settings())
            });

        LOGI("has_pending=%d", perms.has_pending());
    }

    void on_tick() override {
        App::on_tick();     // mantém Permissions::update() rodando a cada frame
    }

    void on_resume() override {
        // O App já chamou notify_activity_result() antes deste callback.
        LOGI("on_resume (notify_activity_result)");
    }

    void on_surface_created() override {}
    void on_draw() override {}

    static constexpr const char* permissions_[] = {
        "android.permission.CAMERA",
        "android.permission.RECORD_AUDIO",
        "android.permission.ACCESS_FINE_LOCATION",
    };
};

APP_MAIN(PermsApp)
```

Manifesto correspondente: o trecho XML da seção 9 (com as três
`<uses-permission>`; `package="com.apkfast.permstest"`).

### 12.1 Variante raylib (loop manual)

```cpp
extern "C" android_app* GetAndroidApp(void);   // declarado em rcore_android.c

android_app* app = GetAndroidApp();
auto& perms = nativeapp::Permissions::instance(app->activity);
perms.request_async({"android.permission.CAMERA", "android.permission.RECORD_AUDIO"},
    [](const nativeapp::PermissionResult& result) {
        __android_log_print(ANDROID_LOG_INFO, "raylib-perms",
                            "all_granted=%d permanent=%d",
                            result.all_granted, result.permanently_denied);
    });

while (!WindowShouldClose()) {
    perms.update();        // necessário: não há App::on_tick() aqui
    // ... desenhar ...
}
```

### 12.2 Variante SDL3 (loop manual)

```cpp
JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
auto& perms = nativeapp::JniPermissions::instance(env, activity);

perms.request_async({"android.permission.CAMERA", "android.permission.RECORD_AUDIO"},
    [](const nativeapp::PermissionResult& result) {
        SDL_Log("all_granted=%d permanent=%d",
                result.all_granted, result.permanently_denied);
    });

while (running) {
    perms.update();        // processa o callback quando a Activity volta
    while (SDL_PollEvent(&event)) { /* ... */ }
    // ... renderizar ...
}
```

---

## 13. Validação no dispositivo (`dumpsys` / `logcat`)

Tags de log dos exemplos: `perms-test`, `raylib-perms`, `sdl3-perms`.

```sh
# Acompanhar os logs dos exemplos
adb logcat -s perms-test:I raylib-perms:I sdl3-perms:I

# Estado das permissões do pacote (dumpsys)
adb shell dumpsys package com.apkfast.permstest | grep -i -A 4 \
  'android.permission.CAMERA\|android.permission.RECORD_AUDIO\|android.permission.ACCESS_FINE_LOCATION'

# Alternativa via pm dump
adb shell pm dump com.apkfast.permstest | grep -A 12 'runtime permissions'
```

O que esperar no `logcat` quando o diálogo termina (trechos impressos pelo
exemplo `permissions-test`):

```text
perms-test: runtime_permissions_available=1
perms-test: package=com.apkfast.permstest
perms-test: antes: android.permission.CAMERA = DENIED
perms-test: === CALLBACK ===
perms-test: all_granted=1 rationale=0 permanent=0
perms-test:   GRANTED:   android.permission.CAMERA
```

Para exercitar a negação e o estado permanente:

```sh
adb shell pm revoke com.apkfast.permstest android.permission.CAMERA
adb shell pm grant  com.apkfast.permstest android.permission.CAMERA
# Para simular um estado "limpo" (as tags do manifesto continuam valendo):
adb shell pm clear com.apkfast.permstest
```

Ao testar `PermanentlyDenied` manualmente no diálogo, use a opção
"Não perguntar novamente": o callback deve trazer `permanent=1` e a permissão em
`denied`. Depois, `openAppSettings()` (ou `open_app_settings()` em `Permissions`)
abre a tela de detalhes do aplicativo para o usuário reabilitá-la.

---

## 14. Resumo de arquivos

| Arquivo | Conteúdo |
|---|---|
| `include/nativeapp/permission_types.hpp` | `PermissionState`, `PermissionResult` |
| `include/nativeapp/permissions.hpp` | `Permissions` (ANativeActivity*) |
| `include/nativeapp/permissions_jni.hpp` | `JniPermissions` (JNIEnv* + jobject) |
| `include/nativeapp/nativeapp.hpp` | Umbrella que inclui ambos |
| `include/nativeapp/app.hpp` | `on_tick()` → `update()`; `APP_CMD_RESUME` → `notify_activity_result()`; `finish()` → `shutdown()` |
| `examples/permissions-test/jni/main.cpp` | Exemplo completo com `nativeapp::App` |
| `examples/raylib-permissions/jni/main.cpp` | Exemplo com `GetAndroidApp()->activity` |
| `examples/sdl3-permissions/jni/main.cpp` | Exemplo com `SDL_GetAndroidJNIEnv()` / `SDL_GetAndroidActivity()` |
| `examples/*/AndroidManifest.xml` | Declarações `<uses-permission>` usadas pelos exemplos |
