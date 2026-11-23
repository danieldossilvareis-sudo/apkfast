# Guia completo do subsistema Vulkan (`nativeapp/vk.hpp`)

> **Fonte de verdade**: `include/nativeapp/vk.hpp`, complementado por
> `include/nativeapp/graphics_types.hpp`, `include/nativeapp/math.hpp`,
> `include/nativeapp/assets.hpp`, `include/nativeapp/app.hpp` (apenas para o
> ciclo de vida) e pelo exemplo `examples/nativeapp-vulkan/jni/main.cpp`.
> Detalhes de build foram confirmados em `apkfast/builder.py`,
> `apkfast/toolchain.py`, `apkfast/cli.py` e `README.md`.
>
> Todo comportamento descrito existe no código. Quando o texto tira uma
> consequência do código (em vez de transcrever um comentário), isso aparece
> como **Observação**. Quando uma limitação honesta da implementação atual
> precisa ser destacada, isso aparece como **Atenção**.

---

## 1. Visão geral: o que `vk.hpp` entrega

`vk.hpp` é um cabeçalho único, em `namespace nativeapp`, que encapsula Vulkan
para uma `NativeActivity`. Ele foi desenhado para ser **compatível com a
interface `Scene`** (`draw` / `present` / `set_clear_color` / `resize` /
`begin_frame`), permitindo trocar o backend GL pelo VK com mudanças mínimas no
app.

O próprio arquivo define três camadas:

```text
  VulkanScene  — instance / device / surface / swapchain / depth /
                 render pass / framebuffers / 2 frames in-flight
       │
       ├── GraphicsPipeline — builder de graphics pipeline (shaders SPIR-V + layout de vértices)
       │
       ├── GpuBuffer        — vertex/index buffer criado a partir de MeshData
       │
       └── Scene            — interface comum (draw/present/set_clear_color)
```

Inclua de uma destas duas formas:

```cpp
#include <nativeapp/nativeapp.hpp>  // umbrella: inclui vk.hpp, app.hpp, assets.hpp etc.
// ou
#include <nativeapp/vk.hpp>         // puxa assets.hpp, graphics_types.hpp, math.hpp,
                                    // <vulkan/vulkan.h>, <vulkan/vulkan_android.h>,
                                    // android/native_activity.h e android/native_window.h
```

### 1.1 O que a cena entrega pronta

| Recurso | Onde é criado | O que o código faz |
|---|---|---|
| `VkInstance` | `create_instance()` | API Vulkan 1.0, app name `"apkfast nativeapp"`, extensões `VK_KHR_SURFACE_EXTENSION_NAME` e `VK_KHR_ANDROID_SURFACE_EXTENSION_NAME` |
| `VkPhysicalDevice` | `pick_physical_device()` | Enumera devices; prefere `VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`, senão usa o primeiro |
| `VkDevice` + `VkQueue` | `create_device()` | Primeira família com `VK_QUEUE_GRAPHICS_BIT`, 1 fila prioridade 1.0, extensão `VK_KHR_SWAPCHAIN_EXTENSION_NAME` |
| `VkSurfaceKHR` | `create_surface()` | `vkCreateAndroidSurfaceKHR` com o `ANativeWindow*` atual |
| `VkSwapchainKHR` | `create_swapchain()` | Formato/present mode/extent consultados da surface; image views; `COLOR_ATTACHMENT \| TRANSFER_DST` |
| Depth | `create_depth_resources()` | Uma imagem `DEPTH_STENCIL_ATTACHMENT`, memória `DEVICE_LOCAL` e image view; formato D32 → D24 → D16 |
| Render pass | `create_render_pass()` | 1 subpass gráfico com anexo de cor (CLEAR/STORE/PRESENT_SRC) e depth (CLEAR/DONT_CARE) |
| Framebuffers | `create_framebuffers()` | Um por imagem da swapchain, sempre `[color_view, depth_view]` |
| 2 frames in-flight | `create_command_buffers()` + `create_sync_objects()` | 2 command buffers, 2 pares de semáforos, 2 fences, mais um fence por imagem da swapchain |
| Pipeline de geometria | `GraphicsPipeline::build()` via `set_pipeline()` | 2 estágios SPIR-V, 4 atributos de vértice, depth/blend/topology configuráveis |
| Upload de mesh | `GpuBuffer::upload()` via `draw()` | Buffers de vértice e índice a partir de `MeshData` |
| Loop de quadro | `render()` | wait fence → acquire → record → submit → present → próximo frame |
| Recuperação | `render()`, `resize()`, `attach()` | Recria swapchain em `OUT_OF_DATE`/`SUBOPTIMAL`, invalida em `DEVICE_LOST` |

### 1.2 O que **não** entrega

É importante ser honesto sobre o que não existe no código:

- **Não há compilador de shaders.** `PipelineConfig` recebe **bytecode SPIR-V
  já compilado** (`std::vector<uint32_t>`). O projeto não inclui `glslc` nem
  `glslangValidator`, nem compila GLSL em runtime.
- **Não há texturas.** `Material::texture` existe em `graphics_types.hpp`, mas
  `VulkanScene::draw()` faz `(void)material` — material, cor e nome de textura
  são ignorados.
- **Não há descriptor sets próprios.** `set_pipeline()` chama
  `pipeline_.build(device_, render_pass_, nullptr, pipeline_config_)`, ou seja,
  sempre passa `nullptr` como `VkDescriptorSetLayout`. `GraphicsPipeline::build()`
  aceita um layout, mas a cena nunca cria um. O próprio código marca uniformes
  como trabalho futuro: `// uniforms via descriptor sets em versão futura`.
  Consequentemente, a matriz `model` passada a `draw()` também é ignorada
  (`(void)model`).
- **Não há sampler, uniform buffer, pipeline de compute, MSAA, instancing,
  stencil, cull configurável, dynamic viewport** nem carregamento de imagens.
- **Não há validação ativa.** `set_validation(bool)` apenas atribui ao campo
  `validation_`; `create_instance()` não habilita layers nem cria
  `VkDebugUtilsMessenger`. **Atenção:** chamar `set_validation(true)` não muda
  nada no comportamento atual.

### 1.3 Tipos de `graphics_types.hpp` consumidos pelo subsistema

| Tipo | Definição | Uso no Vulkan |
|---|---|---|
| `Vertex` | `x,y,z`; `nx,ny,nz` (default `0,0,1`); `u,v`; `r,g,b,a` (default `1,1,1,1`) — 12 floats | É exatamente o layout de 4 atributos do pipeline; `sizeof(Vertex)` é o stride do binding 0 |
| `SpriteVertex` | `x,y,u,v,r,g,b,a` | **Declarado, mas não usado por `vk.hpp`** — `MeshData::vertices` é `std::vector<Vertex>` |
| `Topology` | `Triangles`, `TriangleStrip`, `TriangleFan`, `Lines`, `LineStrip`, `Points` | `PipelineConfig::topology` é traduzido para `VkPrimitiveTopology` pelo `vk_topology()` privado |
| `MeshData` | `vertices`, `indices` (`uint16_t`), `topology`, `add_triangle()`, `add_quad()`, `quad()`, `triangle()` | Entrada de `GpuBuffer::upload()` e de `VulkanScene::draw()` |
| `Material` | `color[4]`, `texture`, `depth_test`, `blend` | Recebido por `draw()`, mas ignorado |
| `Scene` | `resize`, `begin_frame`, `present`, `set_clear_color`, `draw` | `VulkanScene` implementa esta interface |

De `math.hpp`: `Mat4` (`std::array<float,16>`, column-major) e as funções
`mat4_identity`, `mat4_multiply`, `mat4_translate`, `mat4_scale`,
`mat4_rotate_x/y/z`, `mat4_ortho`, `mat4_ortho2d`, `mat4_perspective`
(profundidade 0..1, estilo Vulkan), `mat4_perspective_gl` (-1..1) e
`mat4_look_at`. **Nenhuma matriz é aplicada por `VulkanScene::draw()`** — elas
servem para o seu próprio código caseiro de uniformes (ainda sem suporte na
cena).

### 1.4 Erros

```cpp
class VulkanError : public std::runtime_error {
public:
    explicit VulkanError(const std::string& message);
};

inline void vk_check(VkResult result, const char* operation);
```

`vk_check()` lança `VulkanError` com mensagem
`"operação falhou (VkResult=N)"` para qualquer `VkResult != VK_SUCCESS`.
Ele é usado nas chamadas de criação e submissão. Exceções não tratadas em
callbacks do `App` são problemáticas no NDK (ver `README.md`, item "exceção
C++ sem catch em `on_ready()`"); envolva a inicialização em `try/catch` se
quiser diagnóstico amigável.

---

## 2. Requisitos e build

### 2.1 API 24+

Vulkan no Android está disponível a partir da **API 24 (Android 7.0)**. O
código usa extension sets Android (`VK_KHR_ANDROID_SURFACE_EXTENSION_NAME`,
`vkCreateAndroidSurfaceKHR`) e espera que o dispositivo forneça
`libvulkan.so` em runtime.

**Atenção:** o apkfast **não impõe** esse mínimo automaticamente. O padrão do
scaffold e do CLI é `minSdkVersion=21` (`apkfast/scaffold.py`,
`apkfast/cli.py`, flag `--min-sdk`). Para um app Vulkan, peça explicitamente:

```sh
python3 apkfast.py init --min-sdk 24 ...
python3 apkfast.py build --min-sdk 24 ...
```

Ou ajuste `android:minSdkVersion` no seu manifesto.

### 2.2 `-lvulkan` é adicionado pelo builder

O builder detecta automaticamente o uso de Vulkan procurando estes tokens no
código-fonte (`apkfast/builder.py`):

```python
uses_vulkan = any(
    token in source_text
    for token in ("nativeapp/vk.hpp", "nativeapp/nativeapp.hpp",
                  "vulkan/vulkan.h", "VulkanScene")
)
```

Quando detectado, ele linka:

```python
cmd += ["-Wl,--no-as-needed", "-lvulkan", "-Wl,--as-needed"]
```

E **desativa LTO**:

```python
use_lto = options.optimize != "none" and not uses_vulkan
```

Você não precisa escrever `-lvulkan` na mão. Basta incluir `vk.hpp` ou usar
`VulkanScene`.

### 2.3 O stub NDK **não** é embutido

O `-lvulkan` usa apenas o **stub de linkagem** do NDK (localizado pelo
`Toolchain::ndk_shared_lib()`, que procura inclusive em diretórios de API,
como `libvulkan.so` do NDK r30). Esse stub **não entra no APK**: em runtime, o
carregador nativo do Android fornece a implementação real do dispositivo.

### 2.4 Por que `DT_NEEDED libvulkan.so` é crítico

Já aconteceu (NDK r30 + `-flto`): o linker aplicou `--as-needed`, removeu a
entrada `DT_NEEDED libvulkan.so` e deixou os símbolos `vk*` como `UND`. O
resultado foi falha de `dlopen` com `cannot locate symbol vkCreateInstance`
— tela preta / app fecha ao abrir.

O builder corrige isso de duas formas: desliga LTO quando Vulkan é detectado
e força a entrada com `-Wl,--no-as-needed -lvulkan -Wl,--as-needed`.
Sempre confirme depois de buildar:

```sh
llvm-readelf -d libmain.so | grep libvulkan.so   # deve aparecer DT_NEEDED
```

Se a linha não aparecer, o app vai linkar, mas vai quebrar no carregamento.

---

## 3. Ciclo de inicialização: `init` → `attach` → `invalidate` → recreate

### 3.1 `init(ANativeActivity*)`

```cpp
void init(ANativeActivity* activity);
void init(ANativeActivity* activity, ANativeWindow* window);  // init() + attach()
```

`init(activity)` é idempotente (`if (initialized_) return;`) e, nesta ordem:

1. guarda `activity_`;
2. chama `Assets::instance(activity).set_activity(activity)` — garante que
   `Assets::read()` funcione para carregar SPIR-V de `assets/`;
3. `create_instance()`;
4. `pick_physical_device()`;
5. `create_device()`;
6. marca `initialized_ = true`.

**Observação:** nenhuma surface, swapchain ou render pass é criada aqui. O
device Vulkan sobrevive a ciclos de janela; a swapchain, não.

### 3.2 `attach(ANativeWindow*)`

```cpp
void attach(ANativeWindow* window);
```

Só faz algo se `initialized_ == true` e `window != nullptr`. Se já existe
surface para a **mesma** janela e a swapchain está viva, retorna cedo. Caso
contrário:

```text
invalidate()                 // destrói surface + swapchain antigos, zera window_
window_ = window
create_surface()             // VkAndroidSurfaceCreateInfoKHR
create_swapchain()           // inclui depth, render pass, framebuffers e sync objects
create_command_buffers()     // pool + 2 command buffers
swapchain_ok_ = true
```

**Observação:** a ordem importa. `create_swapchain()` cria os objetos de
sincronização; `create_command_buffers()` cria o pool usado depois por
`GpuBuffer::upload()` e pelos command buffers de `render()`.

### 3.3 `invalidate()` e `destroy()`

```cpp
void invalidate();   // libera tudo que depende da janela
void destroy();      // libera tudo, inclusive device/instance
```

`invalidate()`:

- `if (device_) vkDeviceWaitIdle(device_)`;
- `destroy_swapchain()` (fences, semáforos, command pool, framebuffers, render
  pass, depth, image views, swapchain);
- `vkDestroySurfaceKHR(instance_, surface_, nullptr)` se houver surface;
- `window_ = nullptr`;
- `swapchain_ok_ = false`.

`destroy()`:

- se `!initialized_`, retorna;
- `invalidate()`;
- destrói `render_pass_` se ainda existir;
- `pipeline_.destroy()`;
- `current_mesh_.destroy()`;
- `vkDestroyDevice()` e `vkDestroyInstance()`;
- `initialized_ = false`.

**Observação:** `invalidate()` preserva o dispositivo e a instância; por isso
é o callback correto para `on_surface_destroyed()`. `destroy()` é para o fim
da cena/app.

### 3.4 Quem chama o quê no `App`

O `App` (`include/nativeapp/app.hpp`) dispara, em `activate_surface()`:

1. na **primeira** janela válida: injeta `Assets` e `TextInput`, chama
   `on_ready()` uma única vez e marca `Ready`;
2. quando a janela é nova (`app_->window` mudou ou surface inativa): chama
   `on_surface_created()` e, **na criação**, também `on_surface_changed(w,h)`;
3. em mudanças de tamanho/redraw: `on_surface_changed(w,h)`.

Em `deactivate_surface()`, o `App` **zera `native_window_` antes** de chamar
`on_surface_destroyed()` — `window()` devolve `nullptr` nesse momento. Em
`APP_CMD_RESUME` / `APP_CMD_GAINED_FOCUS`, `activate_surface()` é rechamado se a
janela mudou.

O exemplo `examples/nativeapp-vulkan/jni/main.cpp` mostra o contrato:

| Callback do `App` | O que o exemplo faz | Por quê |
|---|---|---|
| `on_ready()` | `scene_.init(activity())` + `scene_.set_clear_color(...)` | Device criado uma única vez; aqui já há `ANativeActivity` e `AssetManager` |
| `on_surface_created()` | `scene_.attach(window())` | Surface/swapchain são (re)criadas toda vez que a janela volta |
| `on_surface_destroyed()` | `scene_.invalidate()` | Janela morreu; device pode ficar |
| `on_surface_changed(w,h)` | `scene_.resize(w,h)` | `resize()` chama `recreate_swapchain()` |
| `on_draw()` | `scene_.render()` | `present()` também chama `render()`; não chame os dois |
| `on_pause()` / `on_resume()` | apenas log | Não precisam mexer na cena |

Esqueleto mínimo (idêntico ao exemplo, com os logs removidos):

```cpp
class MeuApp : public App {
public:
    explicit MeuApp(android_app* app) : App(app), scene_() {}

private:
    void on_ready() override {
        scene_.init(activity());
        scene_.set_clear_color(0.1f, 0.2f, 0.35f, 1.0f);
    }
    void on_surface_created() override { scene_.attach(window()); }
    void on_surface_destroyed() override { scene_.invalidate(); }
    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
    void on_draw() override { scene_.render(); }

    VulkanScene scene_;
};

APP_MAIN(MeuApp)
```

### 3.5 `resize()` e `recreate_swapchain()`

```cpp
void resize(int, int) override;  // parâmetros são ignorados
```

`resize(w,h)` **ignora os argumentos** e, se `initialized_ && window_`, chama
`recreate_swapchain()`. O tamanho real vem de
`vkGetPhysicalDeviceSurfaceCapabilitiesKHR()` (`capabilities.currentExtent`)
e, se `currentExtent.width == UINT32_MAX`, de
`ANativeWindow_getWidth(window_)` / `ANativeWindow_getHeight(window_)`.

`recreate_swapchain()` preserva a janela explicitamente:

```cpp
ANativeWindow* saved_window = window_;
if (ANativeWindow_getWidth(saved_window) <= 0 ||
    ANativeWindow_getHeight(saved_window) <= 0) return;   // nada a fazer
invalidate();                 // zera window_, swapchain_ok_
window_ = saved_window;       // restaura o ponteiro salvo
create_surface();
create_swapchain();
create_command_buffers();
swapchain_ok_ = true;
if (pipeline_config_.vertex_spv.empty()) {
    // modo "só clear"
} else {
    pipeline_.build(device_, render_pass_, nullptr, pipeline_config_);
}
```

**Observação:** quando há pipeline customizado, ele é reconstruído porque o
render pass foi destruído em `destroy_swapchain()`. Sem pipeline customizado,
a cena volta ao modo clear. A janela é preservada porque `invalidate()` a
descarta — daí o `saved_window`.

---

## 4. `GraphicsPipeline` — builder de graphics pipeline

### 4.1 `PipelineConfig`

```cpp
struct PipelineConfig {
    std::vector<uint32_t> vertex_spv;     // bytecode SPIR-V compilado
    std::vector<uint32_t> fragment_spv;
    bool depth_test = true;               // depthTestEnable + depthWriteEnable
    bool blend = true;                    // blendEnable na attachment 0
    Topology topology = Topology::Triangles;
    VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_D16_UNORM;
};
```

**Atenção:** os campos `color_format` e `depth_format` fazem parte da struct,
mas `GraphicsPipeline::build()` **não os lê**. O render pass é quem define os
formatos reais das attachments, e a cena cria esse render pass com o formato
escolhido da surface e o depth selecionado por `create_depth_resources()`.
Para um pipeline criado via `set_pipeline()`, basta preencher `vertex_spv`,
`fragment_spv`, `depth_test`, `blend` e `topology`.

### 4.2 API pública

```cpp
class GraphicsPipeline {
public:
    GraphicsPipeline() = default;
    ~GraphicsPipeline() { destroy(); }
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    void build(VkDevice device, VkRenderPass render_pass,
               VkDescriptorSetLayout descriptor_layout,
               const PipelineConfig& config);

    void bind(VkCommandBuffer command_buffer) const;
    VkPipelineLayout layout() const;
    VkPipeline pipeline() const;
    void destroy();
};
```

`build()` começa chamando `destroy()` (rebuild seguro), guarda `device_`, cria
os dois `VkShaderModule`, monta os estados e chama `vkCreatePipelineLayout` +
`vkCreateGraphicsPipelines`. Ao final, destrói os dois módulos — o pipeline já
possui o bytecode. Se `build()` falhar no meio, `VulkanError` é lançado por
`vk_check()`.

`bind()` só faz alguma coisa se `pipeline_` existir:

```cpp
vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
```

`layout()` devolve o `VkPipelineLayout` criado; `pipeline()`, o `VkPipeline`.
`destroy()` destrói pipeline e layout e zera `device_`.

### 4.3 Os 4 atributos de vértice (fixos)

O binding 0 usa `stride = sizeof(Vertex)` e `VK_VERTEX_INPUT_RATE_VERTEX`.
Os atributos são exatamente:

| Location | Formato | Campo de `Vertex` | Offset |
|---|---|---|---|
| 0 | `VK_FORMAT_R32G32B32_SFLOAT` | `x` (posição) | `offsetof(Vertex, x)` |
| 1 | `VK_FORMAT_R32G32B32_SFLOAT` | `nx` (normal) | `offsetof(Vertex, nx)` |
| 2 | `VK_FORMAT_R32G32_SFLOAT` | `u` (UV) | `offsetof(Vertex, u)` |
| 3 | `VK_FORMAT_R32G32B32A32_SFLOAT` | `r` (cor) | `offsetof(Vertex, r)` |

Ou seja: **12 floats por vértice**. Seus shaders devem declarar `location=0`
como `vec3`, `location=1` como `vec3`, `location=2` como `vec2` e
`location=3` como `vec4`. Não existe forma de alterar esse layout sem mudar
`vk.hpp`.

### 4.4 Estados fixados no pipeline

| Estado | Valor |
|---|---|
| Entradas | 2 estágios, vertex e fragment, entry point **`"main"`** nos dois |
| Topology | `vk_topology(config.topology)` (ver tabela abaixo) |
| Rasterização | `VK_POLYGON_MODE_FILL`, `VK_CULL_MODE_BACK_BIT`, `VK_FRONT_FACE_COUNTER_CLOCKWISE`, `lineWidth = 1.0f` |
| Multisample | `VK_SAMPLE_COUNT_1_BIT` |
| Depth/stencil | Se `depth_test`: `depthTestEnable = depthWriteEnable = VK_TRUE`, `VK_COMPARE_OP_LESS`; se `false`, `pDepthStencilState = nullptr` |
| Blend attachment 0 | `blendEnable = config.blend`; máscara RGBA; `srcColor = SRC_ALPHA`, `dstColor = ONE_MINUS_SRC_ALPHA`, `colorOp = ADD`; `srcAlpha = ONE`, `dstAlpha = ZERO` |
| Blend state | `attachmentCount = 1` |
| Layout | 1 descriptor set layout somente se `descriptor_layout != nullptr`; senão 0 |
| Render pass / subpass | o passado em `build()` / subpass 0 |

Tradução de `Topology`:

| `Topology` | `VkPrimitiveTopology` |
|---|---|
| `Triangles` (inclui o `default`) | `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST` |
| `TriangleStrip` | `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP` |
| `TriangleFan` | `VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN` |
| `Lines` | `VK_PRIMITIVE_TOPOLOGY_LINE_LIST` |
| `LineStrip` | `VK_PRIMITIVE_TOPOLOGY_LINE_STRIP` |
| `Points` | `VK_PRIMITIVE_TOPOLOGY_POINT_LIST` |

**Atenção:** `VkViewport viewport{}` e `VkRect2D scissor{}` são zero-inicializados
e passados como estado **fixo** (`viewportCount = 1`, `scissorCount = 1`), sem
`VK_DYNAMIC_STATE_VIEWPORT`/`SCISSOR`. O código não chama `vkCmdSetViewport`
nem `vkCmdSetScissor`. Isso é relevante se você usar `GraphicsPipeline`
diretamente: o viewport gravado fica em `(0,0)` com tamanho `0×0`. O exemplo
`nativeapp-vulkan` usa apenas clear, então isso não aparece nele.

### 4.5 Carregando SPIR-V de `assets/`

`Assets::read()` devolve `std::vector<uint8_t>` (e devolve vetor vazio se o
arquivo não existir ou não puder ser lido). `PipelineConfig` quer
`std::vector<uint32_t>`. Não existe helper de conversão em `vk.hpp` — você
precisa fazer isso no seu app. Um helper correto, alinhado, é:

```cpp
// Função do APP, não da biblioteca.
static std::vector<uint32_t> load_spirv(const char* path, bool& ok) {
    const auto bytes = Assets::instance(activity_global).read(path);
    ok = !bytes.empty() && (bytes.size() % 4) == 0;
    std::vector<uint32_t> words;
    if (!ok) return words;
    words.resize(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}
```

(Substitua `activity_global` pelo `ANativeActivity*`/membro do seu app; a
`Assets::instance(activity)` já recebe o activity em `VulkanScene::init()`.)

Na prática:

```cpp
bool ok_v = false, ok_f = false;
PipelineConfig cfg;
cfg.vertex_spv   = load_spirv("shaders/triangle.vert.spv", ok_v);
cfg.fragment_spv = load_spirv("shaders/triangle.frag.spv", ok_f);
if (ok_v && ok_f) scene_.set_pipeline(std::move(cfg));
```

Gere os `.spv` **fora** do apkfast, com `glslc` ou `glslangValidator` (por
exemplo, `glslc shaders/triangle.vert -o assets/shaders/triangle.vert.spv`).
Eles não são compilados em runtime e nenhum desses compiladores acompanha o
projeto. Coloque os arquivos em `assets/` para que `AAssetManager` os encontre.

---

## 5. `GpuBuffer` — vertex/index buffer

### 5.1 API pública

```cpp
class GpuBuffer {
public:
    GpuBuffer() = default;
    ~GpuBuffer() { destroy(); }
    GpuBuffer(const GpuBuffer&) = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;

    void upload(VkDevice device, VkPhysicalDevice physical_device, VkQueue queue,
                VkCommandPool command_pool, const MeshData& mesh);

    void bind(VkCommandBuffer command_buffer) const;
    void draw(VkCommandBuffer command_buffer) const;
    void destroy();
};
```

### 5.2 `upload()`

`upload()` começa com `destroy()` (upload repetido é seguro), guarda
`device_`, `vertex_count_`, `index_count_` e `topology_` da mesh, e cria:

- vertex buffer se `mesh.vertices` não estiver vazio:
  `size = vertices.size() * sizeof(Vertex)`,
  `usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT`;
- index buffer se `mesh.indices` não estiver vazio:
  `size = indices.size() * sizeof(uint16_t)`,
  `usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT`.

Cada buffer recebe `VK_SHARING_MODE_EXCLUSIVE` e memória alocada com
`find_memory_type(..., VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)`. O dados são copiados com
`vkMapMemory` + `std::memcpy` + `vkUnmapMemory`, depois `vkBindBufferMemory`.
Se nenhum memory type servir, `VulkanError("Nenhum memory type adequado")` é
lançado.

**Atenção — honestidade sobre o "staging":** o comentário no topo diz
`"Vertex/index buffer com upload via staging buffer"`, e `upload()` recebe
`VkQueue queue` e `VkCommandPool command_pool`. Porém, a implementação atual
**não cria buffer de staging, não chama `vkCmdCopyBuffer` e não usa `queue` nem
`command_pool`**; ela mapeia diretamente memória host-visible/coerente. Ou
seja: o caminho real é *map + memcpy*, não um staging transfer.

### 5.3 `bind()` e `draw()`

```cpp
// bind()
if (vertex_buffer_) vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);
if (index_buffer_)  vkCmdBindIndexBuffer(cmd, index_buffer_, 0, VK_INDEX_TYPE_UINT16);

// draw()
if (index_buffer_)       vkCmdDrawIndexed(cmd, index_count_, 1, 0, 0, 0);
else if (vertex_buffer_) vkCmdDraw(cmd, vertex_count_, 1, 0, 0);
```

Índices são **sempre `uint16_t`** (como em `MeshData`). Se não houver índices,
usa `vkCmdDraw` com a contagem de vértices. `topology_` é guardado no move, mas
não é lido por `draw()`: quem decide a topologia é o pipeline.

**Observação:** `bind()` e `draw()` só gravam comandos; eles não submetem nada.
Devem ser chamados dentro de um command buffer ativo — é o que
`VulkanScene::record_command_buffer()` faz.

### 5.4 Move semantics e destruição

- Copy constructor e copy assignment estão **deletados**.
- Move constructor e move assignment são `noexcept`; o assignment primeiro
  chama `destroy()` no destino (salvo auto-atribuição) e depois `move_from()`,
  que transfere device, buffers, memórias, contagens e topology, zerando o
  objeto de origem.
- `destroy()` destrói buffers, libera memórias e zera todos os handles; é
  idempotente (`if (!device_) return;`).

`VulkanScene::draw()` usa exatamente esse mecanismo:

```cpp
GpuBuffer buffers;
buffers.upload(device_, physical_device_, queue_, command_pool_, mesh);
current_mesh_ = std::move(buffers);   // dono agora é a cena
has_mesh_ = true;
```

---

## 6. `VulkanScene` — API pública completa

```cpp
class VulkanScene : public Scene {
public:
    static constexpr int kMaxFramesInFlight = 2;

    VulkanScene() = default;
    ~VulkanScene() override;
    VulkanScene(const VulkanScene&) = delete;
    VulkanScene& operator=(const VulkanScene&) = delete;

    void init(ANativeActivity* activity);
    void init(ANativeActivity* activity, ANativeWindow* window);
    void attach(ANativeWindow* window);
    void invalidate();

    void set_clear_color(float r, float g, float b, float a = 1.0f) override;
    void set_present_mode(VkPresentModeKHR mode);
    void set_validation(bool enabled);
    void resize(int, int) override;

    void begin_frame() override;   // corpo vazio
    void present() override;       // chama render()

    void set_pipeline(PipelineConfig config);
    void draw(const MeshData& mesh, const Material& material,
              const float* model = nullptr) override;
    void render();
    void destroy();
};
```

### 6.1 Métodos de configuração

| Método | Comportamento real |
|---|---|
| `set_clear_color(r,g,b,a=1.0f)` | Atribui `clear_ = {{r,g,b,a}}`; usado em `record_command_buffer()` como `clear_values[0].color`. Default: `{0.08, 0.08, 0.12, 1.0}`. |
| `set_present_mode(VkPresentModeKHR)` | Guarda `present_mode_` e, se `initialized_ && window_`, chama `recreate_swapchain()` imediatamente. Default: `VK_PRESENT_MODE_FIFO_KHR`. |
| `set_validation(bool)` | Apenas guarda `validation_`; não altera a instância (ver §1.2). |
| `resize(int,int)` | Ignora os parâmetros; chama `recreate_swapchain()` se houver janela. |
| `set_pipeline(PipelineConfig)` | Se `!initialized_ || !render_pass_`, retorna sem fazer nada. Caso contrário, move a config para `pipeline_config_` e chama `pipeline_.build(device_, render_pass_, nullptr, pipeline_config_)`. |

### 6.2 `begin_frame()`, `present()` e `render()`

`begin_frame()` é vazio — o clear acontece dentro do render pass. `present()`
é um alias de `render()`. **Escolha um dos dois por frame** (`on_draw()` →
`scene_.render()`, como no exemplo).

`render()` começa com as guardas `initialized_ && swapchain_ok_ && window_`.
Se qualquer uma falhar, não faz nada (sem erro, sem log).

Passo a passo:

1. `frame = current_frame_ % kMaxFramesInFlight` (0 ou 1);
2. `vkWaitForFences(..., in_flight_fences_[frame], VK_TRUE, 1000000000)` —
   espera no máximo **1 segundo** (nanossegundos), depois `vkResetFences`;
3. `vkAcquireNextImageKHR(..., UINT64_MAX, image_available_[frame], VK_NULL_HANDLE, &image_index)`;
4. trata o resultado (ver §7.3);
5. se `images_in_flight_[image_index] != VK_NULL_HANDLE`, espera nesse fence
   com `UINT64_MAX` e associa `images_in_flight_[image_index] = in_flight_fences_[frame]`;
6. `vkResetCommandBuffer(command_buffers_[frame], 0)` e
   `record_command_buffer(command_buffers_[frame], image_index)`;
7. `vkQueueSubmit` com:
   - wait: `image_available_[frame]` em `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT`;
   - sinal: `render_finished_[frame]`;
   - fence: `in_flight_fences_[frame]`;
8. `vkQueuePresentKHR` esperando `render_finished_[frame]`, com `swapchain_`
   e `image_index`;
9. trata o resultado do present;
10. `current_frame_++`.

`record_command_buffer()` (privado):

- `vkBeginCommandBuffer`;
- `clear_values[0].color = clear_`; `clear_values[1].depthStencil = {1.0f, 0}`;
- `pass.renderArea.extent` vem de `ANativeWindow_getWidth/Height(window_)`,
  com fallback `1` quando a dimensão é `<= 0` — **não** do extent da
  swapchain;
- `vkCmdBeginRenderPass(..., VK_SUBPASS_CONTENTS_INLINE)`;
- se `pipeline_.pipeline() && has_mesh_`: `pipeline_.bind()` →
  `current_mesh_.bind()` → `current_mesh_.draw()`;
- `vkCmdEndRenderPass` + `vkEndCommandBuffer`.

### 6.3 `draw()`

```cpp
void draw(const MeshData& mesh, const Material& material,
          const float* model = nullptr) override;
```

Guardas: se `!pipeline_.pipeline() || !swapchain_ok_`, retorna. Depois faz o
upload da mesh para um `GpuBuffer` local e move para `current_mesh_`, marcando
`has_mesh_ = true`.

**Observação:** `material` e `model` são descartados (`(void)`). A chamada
**não desenha na hora** — ela apenas prepara o buffer e o flag; o desenho real
ocorre no próximo `render()`/`present()`. Como o upload é feito a cada
`draw()`, chamadas repetidas com a mesma mesh refazem alocação e cópia.

---

## 7. Detalhes internos que importam

### 7.1 Seleção de formato de depth

`create_depth_resources()` testa, nesta ordem:

1. `VK_FORMAT_D32_SFLOAT`
2. `VK_FORMAT_D24_UNORM_S8_UINT`
3. `VK_FORMAT_D16_UNORM`

Usa `vkGetPhysicalDeviceFormatProperties()` e escolhe o primeiro com
`optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT`.
Se nenhum servir, mantém `depth_format_ = candidates[0]` (`D32_SFLOAT`) — o
código não lança erro nesse caso. A imagem é `VK_IMAGE_TILING_OPTIMAL`,
`VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT`, 1 sample, com memória
`VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT`. A image view usa
`VK_IMAGE_ASPECT_DEPTH_BIT`.

### 7.2 Seleção de dispositivo e fila

- `vkEnumeratePhysicalDevices`; `count == 0` → `VulkanError("Nenhum dispositivo Vulkan encontrado")`.
- Começa com `devices[0]`; percorre a lista e, ao achar
  `VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU`, usa esse e para.
- A fila é a primeira família com `VK_QUEUE_GRAPHICS_BIT`; `queue_family_`
  começa em `0`, então se nenhuma tiver graphics, o código ainda tenta a
  família 0.
- Device: 1 fila, prioridade `1.0f`, extensão `VK_KHR_SWAPCHAIN_EXTENSION_NAME`.
- Surface: `VkAndroidSurfaceCreateInfoKHR { .window = window_ }`.

### 7.3 `OUT_OF_DATE`, `SUBOPTIMAL`, `DEVICE_LOST`

Tanto em `vkAcquireNextImageKHR()` como em `vkQueuePresentKHR()`:

| Resultado | Ação no código |
|---|---|
| `VK_ERROR_OUT_OF_DATE_KHR` **ou** `VK_SUBOPTIMAL_KHR` | `recreate_swapchain()` e `return` — o frame atual é descartado |
| `VK_ERROR_DEVICE_LOST` | `invalidate()` e `return` |
| Qualquer outro | `vk_check()` → lança `VulkanError` |

**Atenção:** `invalidate()` chama `vkDeviceWaitIdle(device_)` se `device_` não
for nulo — inclusive no caminho de device lost. O estado `initialized_`
permanece `true`, então um `attach(window)` posterior pode recriar a parte de
janela; o device perdido, porém, não é recriado por `invalidate()`.

### 7.4 Por que a janela é preservada no recreate

`invalidate()` necessariamente descarta `window_` (para não guardar ponteiro
de janela morta). Mas `recreate_swapchain()` precisa da janela para consultar
capabilities e recriar a surface. Por isso ele salva `window_` **antes** de
`invalidate()` e restaura depois. Ele também aborta silenciosamente se
`ANativeWindow_getWidth/Height(saved_window) <= 0` — isso evita recriar uma
swapchain de tamanho zero durante transições de janela.

### 7.5 Formato, present mode e tamanho da swapchain

- Formato: `formats[0]`; se a lista estiver vazia, fallback
  `{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}`.
  O formato escolhido é copiado para `color_format_` e usado no render pass e
  nas image views.
- Present mode: `present_mode_` (configurável) **se estiver na lista de modos
  suportados**; senão cai para `VK_PRESENT_MODE_FIFO_KHR`, que é o default do
  campo e o único garantido pelo tipo.
- Extent: `capabilities.currentExtent`; se `width == UINT32_MAX`, usa
  `ANativeWindow_getWidth/Height(window_)`.
- Número de imagens: `capabilities.minImageCount + 1`, limitado por
  `capabilities.maxImageCount` quando este for `> 0`.
- `imageUsage = COLOR_ATTACHMENT | TRANSFER_DST`, `preTransform =
  capabilities.currentTransform`, `compositeAlpha = OPAQUE`, `clipped =
  VK_TRUE`, `imageSharingMode = EXCLUSIVE`.

Cada imagem ganha uma `VkImageView` 2D, aspect `COLOR`, 1 mip e 1 layer.

### 7.6 2 frames in-flight e fences por imagem

`create_sync_objects()` cria, para `i < 2`:

- `image_available_[i]` — semáforo sinalizado quando uma imagem da swapchain
  está disponível (`vkAcquireNextImageKHR`);
- `render_finished_[i]` — semáforo sinalizado quando o submit termina e
  consumido pelo present;
- `in_flight_fences_[i]` — fence do frame, criado já sinalizado
  (`VK_FENCE_CREATE_SIGNALED_BIT`) para que o primeiro `vkWaitForFences` não
  trave.

Além disso, `images_in_flight_` tem um fence por imagem da swapchain
(inicialmente `VK_NULL_HANDLE`). Isso resolve a situação em que o frame `f`
tenta usar uma imagem que ainda está sendo lida pelo frame anterior:
`render()` espera o fence daquela imagem antes de regravar o command buffer e
submeter.

`create_command_buffers()` cria um pool com
`VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` e 2 command buffers
primários. Cada frame regrava seu próprio buffer com
`vkResetCommandBuffer(..., 0)`.

### 7.7 Ordem de destruição da swapchain

`destroy_swapchain()` (privado) começa com `vkDeviceWaitIdle(device_)` e depois
destrói, nesta ordem: fences → semáforos → limpa os vetores de sincronização →
command pool → framebuffers → render pass → depth view → depth image → depth
memory → image views → swapchain. Tudo é protegido por `if` e os vetores são
`clear()`ed. `swapchain_ok_` é zerado por `invalidate()`, não por
`destroy_swapchain()` diretamente.

---

## 8. Exemplo completo: inicializar, SPIR-V, pipeline, mesh, resize

O exemplo a seguir é uma extensão didática de
`examples/nativeapp-vulkan/jni/main.cpp`. As funções `load_spirv()` e
`make_mesh()` são do **app**, não da biblioteca; os nomes de arquivos são
ilustrativos (coloque os `.spv` reais em `assets/shaders/`).

```cpp
#include <nativeapp/nativeapp.hpp>
#include <android/log.h>

#include <cstring>   // memcpy para SPIR-V
#include <utility>   // std::move

#define LOG_TAG "meu-vk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

// --- helpers do app ---------------------------------------------------------
static bool load_spirv(ANativeActivity* activity, const char* path,
                       std::vector<uint32_t>& out) {
    const auto bytes = Assets::instance(activity).read(path);
    if (bytes.empty() || (bytes.size() % 4) != 0) {
        LOGI("SPIR-V invalido/ausente: %s", path);
        return false;
    }
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return true;
}

class MeuApp : public App {
public:
    explicit MeuApp(android_app* app)
        : App(app), scene_(), activity_(app->activity) {}

private:
    void on_ready() override {
        // 1) Device uma única vez. Assets já injetado pelo App.
        scene_.init(activity_);
        scene_.set_clear_color(0.1f, 0.2f, 0.35f, 1.0f);

        // 2) SPIR-V compilado externamente (glslc/glslangValidator) em assets/.
        PipelineConfig cfg;
        const bool ok_v = load_spirv(activity_, "shaders/triangle.vert.spv",
                                    cfg.vertex_spv);
        const bool ok_f = load_spirv(activity_, "shaders/triangle.frag.spv",
                                    cfg.fragment_spv);
        if (ok_v && ok_f) {
            cfg.depth_test = true;
            cfg.blend = false;
            cfg.topology = Topology::Triangles;
            // color_format/depth_format vêm do render pass da cena.
            scene_.set_pipeline(std::move(cfg));
        } else {
            LOGI("Sem shaders: modo 'so clear'");
        }
    }

    void on_surface_created() override {
        LOGI("surface: %dx%d", width(), height());
        // 3) Surface + swapchain + depth + render pass + framebuffers.
        scene_.attach(window());
    }

    void on_surface_destroyed() override {
        // 4) Janela morreu: libera surface/swapchain, mantém o device.
        scene_.invalidate();
    }

    void on_surface_changed(int w, int h) override {
        LOGI("resize: %dx%d", w, h);
        // 5) Recria swapchain e reconstrói o pipeline automaticamente.
        scene_.resize(w, h);
    }

    void on_draw() override {
        // 6) Mesh CPU-side; o upload acontece em draw().
        static const MeshData mesh = MeshData::triangle();
        static const Material material;  // ignorado por VulkanScene::draw()
        scene_.draw(mesh, material, nullptr);

        // 7) Grava + submete + apresenta (2 frames in-flight internamente).
        scene_.render();
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    ANativeActivity* activity_ = nullptr;
    VulkanScene scene_;
};

APP_MAIN(MeuApp)
```

Para gerar os arquivos de shader (fora do apkfast):

```sh
glslc shaders/triangle.vert -o assets/shaders/triangle.vert.spv
glslc shaders/triangle.frag -o assets/shaders/triangle.frag.spv
```

**Observação:** chamar `scene_.draw()` todo frame funciona, mas refaz o upload
da mesh. Como a cena já guarda `current_mesh_`, uma otimização simples no seu
app é chamar `draw()` apenas quando a geometria muda e `render()` a cada frame.

---

## 9. Tabela completa de classes e métodos

### 9.1 `nativeapp::VulkanError` e `vk_check`

| Item | Assinatura | Papel |
|---|---|---|
| classe | `class VulkanError : public std::runtime_error` | Exceção para falhas Vulkan |
| construtor | `explicit VulkanError(const std::string& message)` | Repassa a mensagem |
| função | `inline void vk_check(VkResult result, const char* operation)` | Lança `VulkanError` se `result != VK_SUCCESS` |

### 9.2 `nativeapp::PipelineConfig`

| Campo | Tipo / default |
|---|---|
| `vertex_spv` | `std::vector<uint32_t>` |
| `fragment_spv` | `std::vector<uint32_t>` |
| `depth_test` | `bool = true` |
| `blend` | `bool = true` |
| `topology` | `Topology = Topology::Triangles` |
| `color_format` | `VkFormat = VK_FORMAT_B8G8R8A8_UNORM` (não lido por `build()`) |
| `depth_format` | `VkFormat = VK_FORMAT_D16_UNORM` (não lido por `build()`) |

### 9.3 `nativeapp::GraphicsPipeline`

| Membro | Assinatura | Observação |
|---|---|---|
| construtor | `GraphicsPipeline() = default` | |
| destrutor | `~GraphicsPipeline() { destroy(); }` | |
| copy | `GraphicsPipeline(const GraphicsPipeline&) = delete` | |
| copy assign | `GraphicsPipeline& operator=(const GraphicsPipeline&) = delete` | |
| `build` | `void build(VkDevice, VkRenderPass, VkDescriptorSetLayout, const PipelineConfig&)` | Chama `destroy()` antes; cria layout + pipeline; destrói os shader modules |
| `bind` | `void bind(VkCommandBuffer) const` | `vkCmdBindPipeline` se houver pipeline |
| `layout` | `VkPipelineLayout layout() const` | |
| `pipeline` | `VkPipeline pipeline() const` | |
| `destroy` | `void destroy()` | Idempotente; pipeline + layout |

Métodos privados: `static VkPrimitiveTopology vk_topology(Topology)` e
`static VkShaderModule create_shader_module(VkDevice, const std::vector<uint32_t>&)`.

### 9.4 `nativeapp::GpuBuffer`

| Membro | Assinatura | Observação |
|---|---|---|
| construtor | `GpuBuffer() = default` | |
| destrutor | `~GpuBuffer() { destroy(); }` | |
| copy / copy assign | deletados | |
| move ctor | `GpuBuffer(GpuBuffer&&) noexcept` | Transfere handles |
| move assign | `GpuBuffer& operator=(GpuBuffer&&) noexcept` | `destroy()` no destino, depois move |
| `upload` | `void upload(VkDevice, VkPhysicalDevice, VkQueue, VkCommandPool, const MeshData&)` | Queue e pool não usados na implementação atual |
| `bind` | `void bind(VkCommandBuffer) const` | Vertex (binding 0) e index (`UINT16`) |
| `draw` | `void draw(VkCommandBuffer) const` | Indexed ou não indexed |
| `destroy` | `void destroy()` | Idempotente |

Métodos privados: `void move_from(GpuBuffer&)`,
`void create_buffer(VkPhysicalDevice, VkDeviceSize, VkBufferUsageFlags, const void*, VkQueue, VkCommandPool, VkBuffer&, VkDeviceMemory&)`,
`uint32_t find_memory_type(VkPhysicalDevice, uint32_t, VkMemoryPropertyFlags) const`.

### 9.5 `nativeapp::VulkanScene` (implementa `Scene`)

| Membro | Assinatura | Observação |
|---|---|---|
| constante | `static constexpr int kMaxFramesInFlight = 2` | |
| construtor | `VulkanScene() = default` | |
| destrutor | `~VulkanScene() override { destroy(); }` | |
| copy / copy assign | deletados | |
| `init` | `void init(ANativeActivity*)` | Idempotente; instance + device |
| `init` | `void init(ANativeActivity*, ANativeWindow*)` | `init()` + `attach()` |
| `attach` | `void attach(ANativeWindow*)` | Recria surface/swapchain se necessário |
| `invalidate` | `void invalidate()` | Libera o que depende da janela |
| `set_clear_color` | `void set_clear_color(float, float, float, float = 1.0f) override` | Cor do clear do anexo 0 |
| `set_present_mode` | `void set_present_mode(VkPresentModeKHR)` | Recria swapchain se houver janela |
| `set_validation` | `void set_validation(bool)` | **Sem efeito** na criação da instância |
| `resize` | `void resize(int, int) override` | Parâmetros ignorados; chama recreate |
| `begin_frame` | `void begin_frame() override` | Corpo vazio |
| `present` | `void present() override` | Chama `render()` |
| `set_pipeline` | `void set_pipeline(PipelineConfig)` | `build(..., nullptr, ...)` |
| `draw` | `void draw(const MeshData&, const Material&, const float* = nullptr) override` | Upload da mesh; material/model ignorados |
| `render` | `void render()` | Acquire → record → submit → present |
| `destroy` | `void destroy()` | Tudo, incluindo device/instance |

Métodos privados: `create_instance`, `pick_physical_device`, `create_device`,
`create_surface`, `create_swapchain`, `create_depth_resources(VkExtent2D)`,
`create_render_pass`, `create_framebuffers(VkExtent2D)`, `create_sync_objects`,
`create_command_buffers`, `record_command_buffer(VkCommandBuffer, uint32_t)`,
`find_memory_type(uint32_t, VkMemoryPropertyFlags) const`, `destroy_swapchain`,
`recreate_swapchain`.

---

## 10. Armadilhas e checklist

1. **SPIR-V precisa ser compilado antes.** `PipelineConfig` não aceita GLSL.
   Use `glslc`/`glslangValidator` (não incluídos no apkfast), coloque os `.spv`
   em `assets/` e converta `Assets::read()` (`uint8_t`) para
   `std::vector<uint32_t>`. Entry point obrigatório: `"main"`.
2. **Formato de arquivo errado.** SPIR-V são palavras de 32 bits; o tamanho em
   bytes deve ser múltiplo de 4. `Assets::read()` devolve vetor vazio em caso
   de falha — sempre verifique antes de montar o pipeline.
3. **`draw()` sem pipeline não desenha.** `draw()` retorna se
   `!pipeline_.pipeline() || !swapchain_ok_`; `record_command_buffer()`
   desenha apenas se `pipeline_.pipeline() && has_mesh_`. Sem
   `set_pipeline()`, a cena fica no modo "só clear" (é o que faz o exemplo
   original).
4. **`draw()` não desenha imediatamente.** Ele só faz upload e marca
   `has_mesh_`. O desenho acontece no `render()`/`present()` seguinte. Chamar
   `present()` **e** `render()` no mesmo frame submete duas vezes.
5. **`set_pipeline()` fora de tempo é silencioso.** Se chamado antes de
   `init()` ou sem render pass, ele retorna sem avisar. Chame-o depois de
   `init()` (como em `on_ready()`).
6. **Resize e device loss.** `OUT_OF_DATE`/`SUBOPTIMAL` →
   `recreate_swapchain()` (o frame é descartado); `DEVICE_LOST` →
   `invalidate()`. `invalidate()` não recria o device e chama
   `vkDeviceWaitIdle()` mesmo nesse caminho. Não tente usar a swapchain
   depois de `invalidate()` sem um `attach()`/`resize()` que a recrie.
7. **`on_surface_destroyed()` não chama `destroy()`.** Use `invalidate()`;
   `window()` já é `nullptr` nesse callback. `destroy()` é para o fim da cena.
8. **Sincronização.** 2 frames alternam por `current_frame_ % 2`; fence do
   frame esperado no máximo 1 s, fence por imagem com espera infinita;
   semáforos `image_available_` → submit → `render_finished_` → present.
   Não assuma que todos os recursos podem ser destruídos logo após
   `draw()` — o upload da mesh substitui `current_mesh_` no próximo `draw()`.
9. **Viewport zero se você usar `GraphicsPipeline` direto.** O pipeline não
   usa dynamic state e inicializa viewport/scissor em zero. A cena lida com o
   desenho internamente, mas não há chamada `vkCmdSetViewport`/`SetScissor`.
10. **Present mode com fallback.** `set_present_mode(MAILBOX/IMMEDIATE)` só
    vale se o modo constar na lista da surface; caso contrário a swapchain é
    criada com `VK_PRESENT_MODE_FIFO_KHR`. `set_present_mode()` também
    dispara recreate imediato se já houver janela.
11. **`DT_NEEDED libvulkan.so` é obrigatório.** Sem ele, `dlopen` falha com
    `cannot locate symbol vkCreateInstance`. Verifique com
    `llvm-readelf -d libmain.so | grep libvulkan.so`. O builder já desliga LTO
    e força o `DT_NEEDED` quando detecta Vulkan; não reative `-flto`
    manualmente nesse caminho.
12. **Stub do NDK ≠ implementação.** Não tente empacotar `libvulkan.so` do
    NDK no APK: o Android fornece a biblioteca real em runtime.
13. **API mínima.** Use `minSdkVersion >= 24`; o padrão do apkfast é 21 e
    ele não valida isso por você.
14. **Sem texturas, sem uniforms, sem matrices aplicadas.** `Material`,
    `model`, `Topology` em `MeshData` (no contexto do pipeline),
    `color_format`/`depth_format` de `PipelineConfig` e `set_validation()` não
    produzem efeito na implementação atual. O layout de vértices e os states são
    fixos em `GraphicsPipeline::build()`; se mudou a geometria, ajuste o pipeline.
15. **`vk_check()` lança exceções.** Trate falhas em `on_ready()` para não
    derrubar a thread nativa sem diagnóstico.

---

### Resumo final

`VulkanScene` dá a infraestrutura Vulkan completa para desenhar uma mesh com
dois frames in-flight e sobreviver a ciclos de janela: `init()` uma vez,
`attach()` por janela, `invalidate()` quando ela morre, `resize()` quando muda,
`draw()` para preparar geometria e `render()`/`present()` para desenhar.
`GraphicsPipeline` fixa um layout de 4 atributos e estados razoáveis, e
`GpuBuffer` faz o upload de `MeshData`. Fora disso — compilação de shaders,
texturas, descriptor sets, uniforms e validação — você mesmo precisa
implementar, ou esperar versões futuras da API.
