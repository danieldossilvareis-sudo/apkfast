# Subsistema Vulkan (`nativeapp/vk.hpp`)

> **Fonte de verdade**: `include/nativeapp/vk.hpp`, complementado por
> `graphics_types.hpp`, `math.hpp`, `font.hpp`, `image_codecs.hpp`,
> `assets.hpp` e pelo exemplo `examples/nativeapp-vulkan/jni/main.cpp`.
>
> Este documento descreve o backend Vulkan após a paridade com `GLES2Scene` e
> a eliminação dos gaps P2 (instancing, uniforms customizados, validação,
> upload assíncrono, cache expansível e modo legado em NDC).

---

## 1. Camadas e conceitos

```text
VulkanScene
  ├── VulkanUploadTracker — staging/fence/semaphore por upload, aposentadoria segura
  ├── SceneUniforms       — UBO std140 embutido (projection/model/color/use_texture)
  ├── TextureVk           — assets → RGBA → staging → VkImage + sampler
  ├── GraphicsPipeline    — SPIR-V embutido + variantes (depth/blend/topologia/instancing)
  ├── GpuBuffer           — MeshData (Vertex/índice) + buffer de instâncias
  └── Scene               — interface comum GL/VK
```

`draw()` **não desenha imediatamente**: enfileira uma cópia da malha, do
material, da matriz `model`, da projeção vigente e do descritor de textura.
O upload e a gravação dos comandos acontecem em `render()`, depois da fence
do frame. Isso evita destruir buffers ainda usados pelos 2 frames in-flight.

---

## 2. API pública

### 2.1 API comum (equivalente a `GLES2Scene`)

```cpp
void init(ANativeActivity* activity);
void init(ANativeActivity* activity, ANativeWindow* window);
void attach(ANativeWindow* window);
void invalidate();
void destroy();
void resize(int w, int h);
void begin_frame() override;        // vazio: clear acontece no render pass
void present() override;            // alias de render()
void render();
void set_clear_color(float r, float g, float b, float a = 1.0f) override;
void draw(const MeshData&, const Material&, const float* model = nullptr) override;
bool load_font(const std::string& path, int pixel_size = 24);
bool has_font() const;
void draw_text(const std::string& text, float x, float y, float scale = 1.0f,
               float r = 1, float g = 1, float b = 1, float a = 1);
void set_projection(const Mat4& projection);
```

### 2.2 Textura, validação e cache

```cpp
bool load_texture(const std::string& path, int svg_w = 0, int svg_h = 0);
bool invalidate_texture(const std::string& path);          // G12
void clear_textures();                                     // G12
bool reload_texture(const std::string& path, int svg_w = 0, int svg_h = 0);
std::size_t texture_count() const;
VulkanUploadContext upload_context() const;
void draw_with_texture(const MeshData&, const Material&, const float* model,
                       const TextureVk* texture_override);

void set_validation(bool enabled);   // ANTES de init(); G13
bool validation_enabled() const;      // layer realmente ativa?
bool device_lost() const;
bool needs_asset_reload() const;
```

`TextureVk`:

```cpp
bool load(const VulkanUploadContext&, const std::string& path,
          int svg_w = 0, int svg_h = 0);
bool load_svg_asset(const VulkanUploadContext&, const std::string& path,
                    int width = 0, int height = 0);
bool load_rgba(const VulkanUploadContext&, const uint8_t* rgba,
               int width, int height);
bool valid() const;
VkImageView view() const;
VkSampler sampler() const;
VkDescriptorSet descriptor() const;
void destroy();
void destroy(const VulkanUploadContext& ctx);  // aposenta sem parar a fila
```

### 2.3 Instancing real (G09)

```cpp
// Um único draw com VkVertexInputBindingDescription.inputRate=INSTANCE.
void draw_instanced(const MeshData& mesh, const Material& material,
                    const std::vector<Mat4>& models);
// Para matrizes geradas por callback: `count` Mat4 column-major contíguas.
void draw_instanced(const MeshData& mesh, const Material& material,
                    const float* model_matrices, std::size_t count);
```

`models[i]` substitui `u_model` do UBO na instância `i` (as colunas da matriz
são atributos de vértice nas locations 4..7, binding 1, divisor 1). O UBO
continua com `projection`/`color`/`use_texture`; `model` fica identidade.
`Material::color` multiplica a cor por vértice de todas as instâncias; para
cores distintas por instância, use um shader customizado com atributo extra.
Vetor vazio não gera draw (mesma política do `GLES3Scene`).

Exemplo:

```cpp
std::vector<Mat4> models;
for (int i = 0; i < 3; ++i) {
    models.push_back(mat4_translate(120.0f * i, 400.0f, 0.0f));
}
Material material;  // color/texture/depth/blend como sempre
scene_.draw_instanced(MeshData::quad(80.0f, 80.0f), material, models);
scene_.render();    // 1 vkCmdDrawIndexed com instanceCount=3
```

### 2.4 Uniforms arbitrários e pipeline customizado (G10/G11)

```cpp
using CustomUniformHandle = std::size_t;  // 0 = nenhum

struct CustomUniformBinding {
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT |
                                VK_SHADER_STAGE_FRAGMENT_BIT;
    std::size_t size = 0;             // bytes do UBO; ignorado por sampler
};

CustomUniformHandle register_custom_uniforms(
    const std::vector<CustomUniformBinding>& bindings);
bool write_uniform(CustomUniformHandle h, uint32_t binding,
                   const void* data, std::size_t size);
bool bind_uniform_texture(CustomUniformHandle h, uint32_t binding,
                          const TextureVk& texture);
VkDescriptorSetLayout custom_uniform_layout(CustomUniformHandle h) const;
VkDescriptorSet custom_uniform_descriptor(CustomUniformHandle h, uint32_t frame) const;
void destroy_custom_uniforms(CustomUniformHandle h);

void set_pipeline(PipelineConfig config);                         // legado
void set_pipeline(PipelineConfig config, CustomUniformHandle h);  // set 2 extra
```

Só `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` e
`VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` são aceitos. Cada UBO recebe um
buffer por frame (alinhado a `minUniformBufferOffsetAlignment`); `write_uniform()`
copia para todos os frames, então não há corrida com frames in-flight.
`destroy_custom_uniforms()` é uma invalidação explícita: espera o device,
libera buffers/layout/sets e reconstrói o pipeline legado sem o set 2.

`PipelineConfig` também expõe `bool instanced` (binding de instância) e
`CustomUniformHandle custom_uniforms`. O layout do pipeline é sempre:
set 0 = UBO embutido, set 1 = sampler embutido e, quando registrado,
set 2 = descritores de usuário.

Exemplo de shader que consome os dois mundos (GLSL 450, SPIR-V fornecido pelo
app):

```glsl
layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 u_projection; mat4 u_model; vec4 u_color; float u_use_texture;
} ubo;                                      // Material/model/color
layout(set = 1, binding = 0) uniform sampler2D u_texture;  // Material::texture
layout(set = 2, binding = 0) uniform CustomUniforms { vec4 u_tint; } custom;
```

```cpp
CustomUniformBinding binding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                             VK_SHADER_STAGE_VERTEX_BIT, 16};
CustomUniformHandle h = scene_.register_custom_uniforms({binding});
float tint[4] = {0.2f, 1.0f, 0.4f, 1.0f};
scene_.write_uniform(h, 0, tint, sizeof(tint));

PipelineConfig cfg;
cfg.vertex_spv = meu_vertex_spv;     // declara set 2 / binding 0
cfg.fragment_spv = meu_fragment_spv; // pode usar set 1 / binding 0
scene_.set_pipeline(cfg, h);
```

**Semântica do legado `set_pipeline()`:** o SPIR-V externo passa a ser criado
com os layouts embutidos (sets 0 e 1) e a cena **sempre** liga o UBO dinâmico
por item e o descritor de textura da material. Portanto `Material::color`,
`Material::texture`, `model` e a projeção são aplicados — o shader só precisa
declarar o bloco acima para consumi-los. SPIR-V que não declara descritores
continua funcionando: os sets extras são ignorados pelo shader. `draw_text()`
sempre usa o pipeline de texto embutido, mesmo no modo legado.

### 2.5 Modo legado de `render()` (G15)

```cpp
void set_legacy_render(bool enabled);
bool legacy_render() const;
void render_legacy();  // one-shot
```

Com `set_legacy_render(true)`, `render()` ignora os itens enfileirados, limpa
o framebuffer, desenha `MeshData::triangle()` (-0.5..0.5) com projeção
**identidade** e apresenta — exatamente como `GLES2Scene::render()`, mesmo que
`set_projection()` tenha recebido uma matriz 3D customizada. A projeção da
cena não é alterada. `render_legacy()` faz o mesmo por um único frame.

Fora desse modo, cada item captura a projeção vigente no `draw()`; trocar
`set_projection()` entre draws afeta apenas os draws seguintes, como no GL.

---

## 3. Shaders embutidos e regeneração

Quatro pipelines embutidos (+ uma variante de vertex para instancing):

| Pipeline | Vertex | Fragment | Uso |
|---|---|---|---|
| Geometria | `kGeometryVertSpv` | `kGeometryFragSpv` | `draw()` |
| Geometria instanciada | `kGeometryInstancedVertSpv` | `kGeometryFragSpv` | `draw_instanced()` |
| Texto | `kTextVertSpv` | `kTextFragSpv` | `draw_text()` |

Os sources GLSL 450 (inclusive `instanced.vert`, com as quatro colunas da
matriz de instância) estão em comentário no header; o SPIR-V foi gerado com:

```sh
glslangValidator -V --target-env vulkan1.0 -S vert geometry.vert -o geometry.vert.spv
glslangValidator -V --target-env vulkan1.0 -S frag geometry.frag -o geometry.frag.spv
glslangValidator -V --target-env vulkan1.0 -S vert text.vert -o text.vert.spv
glslangValidator -V --target-env vulkan1.0 -S frag text.frag -o text.frag.spv
glslangValidator -V --target-env vulkan1.0 -S vert instanced.vert -o instanced.vert.spv
```

Depois converta cada `.spv` para `uint32_t` little-endian e substitua o array
correspondente. `glslangValidator` não é dependência de build.

Bloco uniform (std140, 160 bytes), declarado nos vertex/fragment shaders:

| Campo C++ | Offset | Tamanho |
|---|---|---|
| `Mat4 projection` | 0 | 64 |
| `Mat4 model` | 64 | 64 |
| `float color[4]` | 128 | 16 |
| `float use_texture` | 144 | 4 (+12 padding) |

O offset de cada item é `slot * uniform_stride_`, arredondado para
`minUniformBufferOffsetAlignment`. O descritor é `UNIFORM_BUFFER_DYNAMIC`;
a capacidade começa em 64 slots e cresce sob demanda: nenhum `draw()` aceito
é descartado por teto fixo.

**UVs:** os shaders usam `v_uv = a_uv`. `TextureVk::load_rgba()` grava as
linhas firmemente empacotadas (`bufferRowLength = 0`) e espelha a ordem
vertical do decode, de modo que a primeira linha do PNG/JPEG/WebP/SVG fique
em `v=1`, como em `glTexImage2D`/`MeshData::quad()`. O atlas de fonte passa
pelo mesmo upload; `draw_text()` ajusta só o `v` dos quads de glyph.

---

## 4. Upload de textura assíncrono e descritores

### 4.1 Layouts

| Set | Binding | Tipo | Conteúdo |
|---|---|---|---|
| 0 | 0 | `UNIFORM_BUFFER_DYNAMIC` | UBO do frame (projection/model/color/use_texture) |
| 1 | 0 | `COMBINED_IMAGE_SAMPLER` | white, textura da material ou atlas de fonte |
| 2 | 0..n | UBO estático / sampler | bindings registrados pelo app (pipelines customizados) |

O pool de descritores tem capacidade inicial derivada de `kMaxTextures` com
reserva para white/fonte/externas. Se `vkAllocateDescriptorSets` retornar
`VK_ERROR_OUT_OF_POOL_MEMORY`/`VK_ERROR_FRAGMENTED_POOL`, a cena cria um pool
maior e mantém os antivos vivos enquanto houver sets neles. Sets de UBO,
texturas e uniforms customizados têm o pool que os criou registrado no
descritor, então a liberação é sempre feita no pool correto.

### 4.2 `TextureVk::load_rgba()` sem stall de fila

1. aposenta a imagem anterior (se houver) no `VulkanUploadTracker`;
2. cria imagem `R8G8B8A8_UNORM` `DEVICE_LOCAL` e staging `HOST_VISIBLE|COHERENT`;
3. copia as linhas empacotadas e espelhadas para o staging;
4. em um command buffer one-time: `UNDEFINED → TRANSFER_DST_OPTIMAL`,
   `vkCmdCopyBufferToImage`, `TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL`;
5. cria fence + semáforo, submete a cópia e **retorna imediatamente**;
6. cria view/sampler/descriptor e publica a textura.

O `render()` seguinte inclui os semáforos pendentes em `VkSubmitInfo` com
`pWaitDstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`; a amostragem
nunca corre à frente da cópia. O tracker libera staging, semáforo, fence e
recursos aposentados quando a fence sinaliza e pelo menos um submit posterior
ocorreu. Com 2 frames in-flight e uploads a cada frame, a liberação usa o
contador de submits (o frame que podia amostrar o recurso antigo já foi
reutilizado, e a cena esperou sua fence).

`destroy()` com `VulkanUploadContext` aposenta; `destroy()` sem contexto usa
o tracker guardado no upload e também aposenta — **não** `vkQueueWaitIdle()`.
 Por isso uma `TextureVk` carregada com `scene.upload_context()` deve ser
destruída antes da `VulkanScene` (o descritor pertence ao pool da cena).
O caminho sem tracker (contexto montado à mão, fora da cena) espera só a
fence local; é documentado como síncrono, mas nunca para a fila inteira.

### 4.3 Cache por caminho (G12)

`Material::texture = "sprite.png"` chama `ensure_texture()`: se o caminho
não está no cache, decodifica e sobe uma `TextureVk`. Não há teto rígido;
`kMaxTextures` é apenas a capacidade inicial do pool. `invalidate_texture()`,
`reload_texture()` e `clear_textures()` removem entradas explicitamente e
aposentam os recursos com segurança. Falhas de decode são avisadas no logcat
e caem para a textura branca com `use_texture = 0` — nunca desaparecem em
silêncio por contagem.

---

## 5. Variantes de pipeline

| Material / chamada | Pipeline |
|---|---|
| `depth_test = true`, `blend = true` (default) | depth on + blend `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` |
| `depth_test = false` | depth test/write off |
| `blend = false` | blend off |
| `draw_text()` | layout `x,y,u,v`, blend alpha, depth off |
| `draw_instanced()` | binding 1 `VK_VERTEX_INPUT_RATE_INSTANCE`, SPV instanciado |

Pipelines são criados sob demanda por `(depth, blend, texto, instancing,
topologia)` e reaproveitados. `invalidate()` os destrói porque o render pass
morre com a swapchain. O pipeline externo (`set_pipeline()`) sobrevive a
`resize()` e é reconstruído com os layouts atuais.

---

## 6. Texto

```cpp
if (!scene_.load_font("font.ttf", 28)) {
    // asset ausente ou TTF inválido
}
if (scene_.has_font()) {
    scene_.draw_text("VULKAN OK", 24.0f, 48.0f, 1.4f,
                     1.0f, 0.95f, 0.65f, 1.0f);
}
```

`load_font()` usa `FontAtlas::load()` (ASCII imprimível 32–126), expande o
atlas 1 canal para RGBA e sobe como `TextureVk`. `draw_text()` gera 6 vértices
por glyph com `vk_detail::GlyphVertex { x, y, u, v }`.

`resize(w,h)` instala `vk_detail::ortho2d_screen(w,h)` =
`mat4_ortho(0, w, 0, h, -1, 1)`: origem no canto superior esquerdo e pixels
como unidade no NDC do Vulkan (y=+1 na base). `draw_text()` passa `y`
diretamente a `FontAtlas::quad()` e aplica `v = 1 - v` por causa do
espelhamento do atlas no upload. Com `set_projection()` customizado, texto e
geometria usam a mesma matriz que o GL usaria.

---

## 7. Ciclo de vida, sincronização e validação

Sequência de um frame em `render()`:

1. se `legacy_render_`, descarta itens e enfileira o triângulo em NDC;
2. espera a fence do frame (1 s) e trata `DEVICE_LOST`;
3. replica uploads cujas fences sinalizaram e cujos semáforos já foram
   consumidos por submits anteriores;
4. `vkAcquireNextImageKHR` (recria swapchain em `OUT_OF_DATE`/`SUBOPTIMAL`);
5. espera a fence da imagem, se ela ainda estiver em uso;
6. `prepare_frame()`: cresce o UBO se necessário, faz upload de geometria e
   instâncias e grava os uniforms (com a projeção capturada por item);
7. grava clear + viewport + itens no command buffer;
8. submit com semáforos de imagem **e de uploads** → `render_finished` + fence;
9. `vkQueuePresentKHR` e `current_frame_++`.

Recursos de device (upload pool, layouts, pools de descritores, UBOs,
texturas, fonte, tracker, uniforms customizados) são criados em `init()` e
destruídos em `destroy()`. Recursos ligados ao render pass (pipelines) são
recriados em `attach()`/`recreate_swapchain()`.

### Validação (G13)

`set_validation(true)` **antes de `init()`** enumera as layers disponíveis e
habilita `VK_LAYER_KHRONOS_validation` (ou `VK_LAYER_LUNARG_standard_validation`
em SDKs antigos) na criação da instância. Se `VK_EXT_debug_utils` existir, um
`VkDebugUtilsMessengerEXT` redireciona avisos/erros para o logcat com a tag
`nativeapp-vk-validation`. `validation_enabled()` informa se a layer está
realmente ativa. Se não houver layer ou extensão, a cena continua e registra
o degrade explicitamente no logcat; chamar `set_validation()` depois do
`init()` gera aviso (a instância já foi criada).

**Device loss:** `VK_ERROR_DEVICE_LOST` em `wait/acquire/submit/present` marca
`device_lost()`, impede novos `render()` e descarta os handles sem chamadas
Vulkan no device perdido. O logcat registra o estado, `needs_asset_reload()`
fica true e a cena só volta com nova chamada a `init()` seguida de
`load_font()`/`load_texture()`.

---

## 8. Exemplo e verificação

`examples/nativeapp-vulkan/jni/main.cpp` demonstra clear, triângulo, sprite e
texto. Contém também `demo_p2_api()` com exemplos compilados (instancing,
invalidação de cache, uniforms customizados e validação) que não são chamados
para preservar a cena de paridade visual.

### Assets necessários

```text
examples/nativeapp-vulkan/assets/font.ttf    # qualquer TTF/OTF ASCII
examples/nativeapp-vulkan/assets/sprite.png  # PNG (ou .webp/.jpg/.svg ajustando o código)
```

Sem eles o app continua rodando: loga aviso, desenha o triângulo colorido e
pula textura/texto.

### Build e verificação

```sh
python3 apkfast.py build examples/nativeapp-vulkan -o /tmp/vk-parity.apk -q
sh scripts/device-check.sh /tmp/vk-parity.apk com.apkfast.nativeappvk
sh scripts/visual-parity.sh --out /tmp/visual-parity
python3 -m unittest discover -s tests -q
```

A paridade visual foi reconfirmada em `emulator-5554` (1080×2340): `gl.png` e
`vk.png` com o mesmo clear, triângulo, sprite 180×180 e texto `VULKAN OK` na
orientação correta, sem diferença perceptível.

---

## 9. Tabela de paridade GL × VK

| Recurso | GLES2Scene | VulkanScene |
|---|---|---|
| Clear color | `glClearColor()` em `begin_frame()` | `VkClearValue` no render pass |
| Projeção | `mat4_ortho2d(w,h)` em `resize()` | `vk_detail::ortho2d_screen(w,h)` = `mat4_ortho(0,w,0,h,-1,1)` (NDC VK) |
| `render()` legado | clear + triângulo em NDC + swap | `set_legacy_render(true)`/`render_legacy()` com projeção identidade |
| Geometria | `Mesh` com VBO/IBO | `GpuBuffer` com vertex/index buffer |
| Instancing | `GLES3Scene::draw_instanced()` (loop de draws) | `draw_instanced()` com `inputRate=INSTANCE` e um `vkCmdDrawIndexed` |
| `Material::color` | uniform `u_color` | UBO `SceneUniforms::color` (inclusive no pipeline legado) |
| `Material::texture` | cache `unordered_map<string,Texture>` | cache `unordered_map<string,TextureVk>` expansível + invalidação |
| Textura externa | `draw_with_texture()` | `draw_with_texture()` + `TextureVk::load_rgba(scene.upload_context(),...)` |
| Upload de textura | síncrono (`glTexImage2D`) | staging + fence + semáforo; sem `vkQueueWaitIdle()` |
| Textura branca | `vec4(1)` quando sem textura | `TextureVk` 1×1 + `use_texture = 0` |
| `depth_test`/`blend` | estado GL por draw | variantes de pipeline por draw |
| Matriz `model` | uniform `u_model` | UBO `SceneUniforms::model` / instância |
| Uniforms de usuário | `Shader::set_uniform*` | `register_custom_uniforms()` + `write_uniform()` (set 2) |
| Shader custom | `set_shader()` (GLSL runtime) | `set_pipeline(config)` / `set_pipeline(config, handle)` (SPIR-V) |
| Vários draws/frame | uniforms por chamada | dynamic offsets (capacidade cresce) + sets por textura |
| Fonte | `FontAtlas` + GL_ALPHA→RGBA | `FontAtlas` + `TextureVk` RGBA |
| Orientação UV | UVs GL aplicados como estão | `v_uv = a_uv`; upload espelha linhas; glyph aplica `v = 1 - v` |
| Lifecycle | `init/resize/present/destroy` | `init/attach/invalidate/resize/render/destroy` |
| In-flight | EGL duplo buffer | 2 frames + fences + UBO por frame + uploads com semaphore |

---

## 10. Limitações restantes (fora do contrato comum)

- Sem mipmaps/anisotropia, MSAA ou cull configurável na API da cena
  (`PipelineConfig::cull_mode` existe, default `VK_CULL_MODE_NONE`, como o GL).
- `FontAtlas` cobre apenas ASCII imprimível (32–126), igual ao GL; sem
  kerning/RTL/ligaduras.
- SVG respeita `svg_w`/`svg_h` na sobrecarga de `load_texture()`;
  `Material::texture` usa o tamanho natural do documento.
- Validação depende da layer estar instalada no dispositivo; sem ela a cena
  degrada com aviso e `validation_enabled() == false`.
- O upload sem `VulkanUploadTracker` (contexto construído à mão) espera a
  fence local: útil, porém síncrono na thread chamadora.
- `GraphicsPipeline` direto com `text_layout = true` exige vértices `x,y,u,v`.
- O exemplo não versiona assets; veja a seção 8.
