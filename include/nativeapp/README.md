# nativeapp API

API C++ header-only para criar apps nativos Android (sem Java) com OpenGL ES,
Vulkan e permissões em runtime. O `apkfast` adiciona automaticamente
`-I<apkfast>/include`, compila o `android_native_app_glue` e linka as libs
necessárias.

## Início rápido (GLES2)

```cpp
#include <nativeapp/nativeapp.hpp>

using namespace nativeapp;

class MeuApp : public App {
public:
    explicit MeuApp(android_app* app) : App(app) {}
private:
    GLES2Scene scene_;
    MeshData triangle_ = MeshData::triangle();

    void on_ready() override {
        set_scene(&scene_);  // App chama begin_frame()/present()
        scene_.set_clear_color(0.05f, 0.05f, 0.1f);
    }
    void on_surface_created() override { scene_.init(window()); }
    void on_surface_destroyed() override { scene_.destroy(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
    void on_draw() override {
        Material material;
        scene_.draw(triangle_, material);
    }
    void on_update(float dt) override {
        // lógica por frame com delta time em segundos
    }
};
APP_MAIN(MeuApp)
```

Exemplo completo: `examples/nativeapp-advanced`.

## Máquina de estados do App

```
Created → Ready ↔ Paused → Destroyed
             ↕ surface
     on_surface_created / on_surface_destroyed
```

| Método | Descrição |
|---|---|
| `on_ready()` | Uma vez, antes da primeira janela |
| `on_surface_created()` | Janela criada/recriada (init GL/Vulkan) |
| `on_surface_destroyed()` | Janela destruída (background/rotação) |
| `on_surface_changed(w,h)` | Resize/rotação |
| `on_update(dt)` | Todo frame, com delta time (segundos) |
| `on_draw()` | Todo frame (desenhe aqui) |
| `on_pause()` / `on_resume()` | Background / foreground |
| `on_tick()` | Processa permissões pendentes (automático) |
| `on_touch(action, pts)` / `on_key(code, down)` | Input |
| `set_scene(Scene*)` | App gerencia `begin_frame()`/`present()` |
| `fps()`, `delta_time()`, `frames()` | Métricas |
| `is_key_down(code)`, `is_touch_down(id)`, `last_touch(id)` | Input polling |
| `request_exit()` | Encerra a Activity |
| `lifecycle()`, `is_ready()` | Estado |

## API gráfica

### Tipos comuns (`graphics_types.hpp`)
- `Vertex` — posição, normal, UV, cor
- `SpriteVertex` — versão 2D compacta
- `MeshData` — geometria CPU com `add_triangle()`, `add_quad()`,
  `MeshData::quad(w,h)`, `MeshData::triangle()`
- `Material` — cor RGBA, textura (caminho em `assets/`), depth/blend
- `Topology` — Triangles, TriangleStrip, Lines, Points...
- `Scene` — interface comum GL/Vulkan:
  `resize()`, `begin_frame()`, `draw()`, `present()`, `set_clear_color()`

### Matemática (`math.hpp`)
`Mat4` (column-major), `Vec3`:
- `mat4_identity`, `mat4_multiply`, `mat4_translate`, `mat4_scale`
- `mat4_rotate_x/y/z`, `mat4_ortho`, `mat4_ortho2d`
- `mat4_perspective` (Vulkan 0..1), `mat4_perspective_gl` (-1..1)
- `mat4_look_at`

### OpenGL ES (`gl.hpp`)
| Classe | Descrição |
|---|---|
| `GLContext` | Display/context/surface EGL, `set_swap_interval()`, `recreate_surface()` |
| `Shader` | `compile(src,src)`, `load(assets/vs,assets/fs)`, `set_uniform*()` |
| `Mesh` | VBO + IBO reutilizável, `upload(MeshData)`, `draw()` |
| `Texture` | PNG/JPEG de `assets/` via stb_image, `bind(unit)` |
| `GLES2Scene` | Implementa `Scene`, shaders padrão, `set_shader()`, `set_mesh()`, `set_projection()` |
| `GLES3Scene` | Extende GLES2 com `draw_instanced()` |

Carregamento de assets usa `AAssetManager` (injetado automaticamente pelo App).
`glGetError()` só é chamado em modo debug (`-DAPKFAST_DEBUG`).

### Vulkan (`vk.hpp`)
| Classe | Descrição |
|---|---|
| `GraphicsPipeline` | Builder: vertex/fragment SPIR-V, depth, blend, topology |
| `GpuBuffer` | Upload de `MeshData` para VBO/IBO |
| `VulkanScene` | Instance/device/surface/swapchain/depth/framebuffers, **2 frames in-flight**, `set_pipeline()`, `set_present_mode()` |

Melhorias implementadas:
- **Depth buffer** (D32/D24/D16 conforme suporte)
- **Render pass** com color + depth attachments
- **2 frames in-flight** com fences por imagem (não serializa CPU/GPU)
- **Device selection** prefere GPU discreta
- **Present mode** configurável (FIFO padrão, MAILBOX/IMMEDIATE)
- Tratamento de `VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR`, `VK_ERROR_DEVICE_LOST`
- Device loss/reenvio de swapchain preservam a janela corretamente

Para desenhar geometria no Vulkan, crie shaders `.spv` em `assets/` e chame
`set_pipeline()` com o bytecode SPIR-V.

## Permissões em runtime

`Permissions` (ANativeActivity*) e `JniPermissions` (JNIEnv* + jobject, para
SDL2/SDL3) com `request_async(callback)` disparado no RESUME após o diálogo.

 Estados: `Granted`, `Denied`, `PermanentlyDenied`.

| Engine | API |
|---|---|
| `nativeapp::App` | `Permissions::instance(activity())` |
| raylib | `Permissions::instance(GetAndroidApp()->activity)` |
| SDL2 | `JniPermissions::instance(env, SDL_AndroidGetActivity())` |
| SDL3 | `JniPermissions::instance(SDL_GetAndroidJNIEnv(), SDL_GetAndroidActivity())` |

Exemplos: `examples/permissions-test`, `examples/raylib-permissions`,
`examples/sdl3-permissions`.

## Arquivos do módulo

| Header | Conteúdo |
|---|---|
| `nativeapp.hpp` | Umbrella (inclui tudo) |
| `app.hpp` | Ciclo de vida, input, Scene management |
| `graphics_types.hpp` | Vertex, MeshData, Material, Scene |
| `math.hpp` | Mat4, projeções |
| `assets.hpp` | AAssetManager |
| `gl.hpp` | GLContext, Shader, Mesh, Texture, GLES2/3Scene |
| `vk.hpp` | GraphicsPipeline, GpuBuffer, VulkanScene |
| `permissions.hpp` / `permissions_jni.hpp` / `permission_types.hpp` | Permissões |
| `stb_image.h` | Carregamento de imagem (domínio público) |
