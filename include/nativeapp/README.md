# nativeapp API

API C++ header-only para criar apps `NativeActivity` sem Java, com OpenGL ES,
Vulkan e permissões em runtime. O `apkfast` adiciona automaticamente
`-I<apkfast>/include` e compila o `android_native_app_glue`.

## Início rápido (OpenGL ES)

```cpp
#include <nativeapp/nativeapp.hpp>

using namespace nativeapp;

class MeuApp : public App {
    GLES2Scene scene_;

    void on_ready() override {
        scene_.init(window());
        scene_.resize(width(), height());

        Permissions::instance(activity())
            .request({"android.permission.VIBRATE"});

        set_ready();
    }

    void on_resize(int w, int h) override { scene_.resize(w, h); }
    void on_draw() override { scene_.render(); }
    void on_touch(TouchAction a, const std::vector<TouchPoint>& pts) override {
        if (!pts.empty()) scene_.set_offset(pts[0].x / width(), pts[0].y / height());
    }
};

APP_MAIN(MeuApp)
```

Build:
```sh
python3 apkfast.py build .
```

## Vulkan

Mesmo código, trocando `GLES2Scene` por `VulkanScene`:

```cpp
VulkanScene scene_;
void on_ready() override {
    scene_.init(activity(), window());
    scene_.set_clear_color(0.1f, 0.2f, 0.35f);
    set_ready();
}
void on_draw() override { scene_.render(); }
```

O `apkfast` detecta `#include <nativeapp/vk.hpp>` (ou `vulkan/vulkan.h`),
adiciona `-lvulkan` e embute o stub `libvulkan.so` no APK.

## Permissões em runtime

### Estilo síncrono (simples)

```cpp
auto& perms = Permissions::instance(activity());
auto r = perms.request({"android.permission.CAMERA"});
if (!r.all_granted && r.should_show_rationale) {
    perms.openAppSettings();
}
```

### Estilo assíncrono (recomendado)

```cpp
void on_ready() override {
    Permissions::instance(activity()).request_async(
        {"android.permission.CAMERA", "android.permission.RECORD_AUDIO"},
        [](const PermissionResult& result) {
            if (result.all_granted) {
                // iniciar câmera/microfone
            } else {
                // explicar ao usuário; result.denied contém as negadas
            }
        });
}
```

O `App` já chama `Permissions::update()` a cada frame (`on_tick()`) e
`notify_activity_result()` quando a Activity volta do diálogo (`APP_CMD_RESUME`).
Você **não precisa escrever Java** nem sobrescrever
`onRequestPermissionsResult` — o polling via `is_granted()` detecta a resposta.

- API < 23: permissões são tratadas como já concedidas (modo legado).
- API >= 23: usa `checkSelfPermission` / `requestPermissions` via JNI.
- `permissions.hpp` usa `activity_->env` (thread principal, segura no `on_ready`).
- `-g` no `apkfast install` concede permissões perigosas automaticamente em
  dispositivos de teste: `python3 apkfast.py install app.apk -g`.

## Referência da API

### `App` — ciclo de vida

```
Created -> Ready <-> Paused -> Destroyed
             ↕ surface
     on_surface_created / on_surface_destroyed
```

| Método | Descrição |
|---|---|
| `on_ready()` | Uma vez, na primeira janela (init device/GL uma vez) |
| `on_surface_created()` | Janela criada **ou recriada** (crie swapchain/EGL aqui) |
| `on_surface_destroyed()` | Janela destruída (background/rotação) — invalide a swapchain |
| `on_surface_changed(w,h)` | Resize/rotação com a surface viva |
| `on_draw()` | Chamado a cada frame quando `is_ready()` |
| `on_pause()` / `on_resume()` | App foi para background / voltou |
| `on_touch(action, points)` | Multi-toque com `id/x/y` |
| `on_key(code, down)` | Tecla física (ex.: `AKEYCODE_BACK`) |
| `lifecycle()` | Estado atual (`Lifecycle::Created/Ready/Paused/Destroyed`) |
| `is_ready()` | `true` se pode desenhar |
| `width()` / `height()` / `frames()` | Estado |
| `request_exit()` | Encerra a Activity |
| `APP_MAIN(Classe)` | Define `android_main()` automaticamente |

> `set_ready()` ainda existe por compatibilidade, mas não é mais necessário.
> O loop de render é controlado automaticamente pela máquina de estados.

Exemplo correto para Vulkan:

```cpp
void on_ready() override { scene_.init(activity()); }        // device único
void on_surface_created() override { scene_.attach(window()); }
void on_surface_destroyed() override { scene_.invalidate(); }
void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
void on_draw() override { scene_.render(); }
```

Isso evita a tela preta ao voltar de background: a swapchain é recriada no
`APP_CMD_RESUME`/`APP_CMD_INIT_WINDOW`, e `VulkanScene::render()` também trata
`VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR` e `VK_ERROR_DEVICE_LOST`.

### `GLES2Scene`
| Método | Descrição |
|---|---|
| `init(window)` | Configura EGL + contexto GLES2 + shader padrão |
| `resize(w,h)` | `glViewport` |
| `set_vertices(vector<Vertex>)` | Triângulo customizado (x,y,r,g,b) |
| `set_offset(x,y)` | Move a geometria (NDC) |
| `render()` | Limpa e desenha |
| `destroy()` | Libera recursos (automático no destrutor) |

### `VulkanScene`
| Método | Descrição |
|---|---|
| `init(activity)` | Instance/physical device/device (uma vez) |
| `init(activity, window)` | Atalho que também chama `attach()` |
| `attach(window)` | Cria/recria surface + swapchain |
| `invalidate()` | Libera swapchain e surface (background) |
| `recreate_swapchain()` | Recria com validação de extent |
| `set_clear_color(r,g,b,a)` | Cor de clear |
| `resize(w,h)` | Recria swapchain (alias) |
| `render()` | Clear + present; trata OUT_OF_DATE / DEVICE_LOST |

### `Permissions`
| Método | Descrição |
|---|---|
| `is_granted(perm)` | Consulta estado |
| `request({perms})` | Solicita; retorna `PermissionResult` |
| `request_async({perms}, callback)` | Solicita e dispara callback quando respondido |
| `update()` | Processa callback pendente (chamado por `App::on_tick`) |
| `notify_activity_result()` | Sinaliza retorno do diálogo (chamado por `App`) |
| `has_pending()` | `true` se há callback aguardando resposta |
| `openAppSettings()` | Abre configurações do app |
| `package_name()` | Nome do pacote |
| `runtime_permissions_available()` | `true` se API >= 23 |

## Notas

- Use `--obfuscate` para esconder símbolos da sua cena (recomendado em release).
- Use `--abi all` para gerar fat APK com todas as arquiteturas.
- Exemplos completos: `examples/nativeapp-gl` e `examples/nativeapp-vulkan`.
- `App::run()` usa `ALooper_pollOnce` com `timeout=0` e limita eventos por
  iteração — isso evita o loop travar antes da janela existir (causa comum
  de tela preta). `activate_window()` reconecta a surface após diálogos de
  permissão, teclado ou rotação.
- `GLES2Scene::set_clear_color(r,g,b,a)` define a cor de fundo.
- Vulkan: use `-lvulkan`, mas **não embuta** `libvulkan.so` do NDK — o Android
  fornece a implementação real em tempo de execução. O apkfast desativa LTO e
  força `-Wl,--no-as-needed -lvulkan` para manter o `DT_NEEDED` (sem isso o
  `dlopen` falha com `cannot locate symbol vkCreateInstance`).
