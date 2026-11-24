# Subsistema Vulkan (`nativeapp/vk.hpp`)

> **Fonte de verdade**: `include/nativeapp/vk.hpp`, complementado por
> `graphics_types.hpp`, `math.hpp`, `font.hpp`, `image_codecs.hpp`,
> `assets.hpp` e pelo exemplo `examples/nativeapp-vulkan/jni/main.cpp`.
>
> Este documento descreve o backend Vulkan após a paridade com `GLES2Scene`.
> Onde uma limitação da implementação atual precisa ser explícita, ela aparece
> como **Atenção**.

---

## 1. O que mudou em relação ao backend antigo

O backend antigo só fazia clear/present; `set_pipeline()` exigia SPIR-V externo
e `draw()` ignorava `Material` e `model`. Agora `VulkanScene` tem:

- **shaders embutidos**: GLSL 450 no comentário do header e SPIR-V como arrays
  `uint32_t` (`kGeometryVertSpv`, `kGeometryFragSpv`, `kTextVertSpv`,
  `kTextFragSpv`). Nenhum `.spv` em `assets/` é necessário para o uso normal.
- **uniform buffer por frame** com `projection`, `model`, `color` e
  `use_texture`, acessado por dynamic offset (um slot por item enfileirado).
- **dois descriptor sets**: set 0 = UBO dinâmico; set 1 = combined image sampler.
- **`TextureVk`**: decode PNG/JPEG/WebP/SVG com `decode_asset_image()` (o mesmo
  do GL), upload via staging buffer `HOST_VISIBLE → DEVICE_LOCAL`, image view,
  sampler linear/clamp e descriptor set próprio.
- **`Material` funcional**: cor, textura por caminho (cache), `depth_test` e
  `blend` selecionam variantes de pipeline.
- **`load_font()` + `draw_text()`**: `FontAtlas` RGBA vira textura Vulkan e
  cada glyph gera dois triângulos com pipeline de texto (blend alpha, depth
  desligado).
- **lifecycle preservado**: `init`, `attach`, `render`, `set_pipeline`,
  `set_clear_color`, `set_present_mode`, `invalidate`, `resize` continuam
  funcionando.

Camadas do header:

```text
VulkanScene
 ├── VulkanUploadContext / SceneUniforms
 ├── TextureVk          — assets → RGBA → staging → VkImage + sampler
 ├── GraphicsPipeline   — SPIR-V embutido, 4 variantes de geometria + texto
 ├── GpuBuffer          — MeshData (Vertex) ou vértices de glyph (x,y,u,v)
 └── Scene              — interface comum GL/VK
```

---

## 2. API pública adicionada

```cpp
// Pré-carrega uma textura de assets/ no cache por caminho.
bool load_texture(const std::string& path, int svg_w = 0, int svg_h = 0);

// Mesma assinatura de GLES2Scene.
bool load_font(const std::string& path, int pixel_size = 24);
bool has_font() const;
void draw_text(const std::string& text, float x, float y,
               float scale = 1.0f,
               float r = 1.0f, float g = 1.0f, float b = 1.0f,
               float a = 1.0f);

// Compatível com GLES2Scene: matriz de projeção em coordenadas de tela.
void set_projection(const Mat4& projection);

// Textura avulsa (também usada internamente pelo cache e pelo atlas).
class TextureVk {
    bool load(const VulkanUploadContext& ctx, const std::string& path,
              int svg_w = 0, int svg_h = 0);
    bool load_svg_asset(const VulkanUploadContext& ctx,
                        const std::string& path, int width = 0, int height = 0);
    bool load_rgba(const VulkanUploadContext& ctx, const uint8_t* rgba,
                   int width, int height);
    bool valid() const;
    int width() const;
    int height() const;
    VkImageView view() const;
    VkSampler sampler() const;
    VkDescriptorSet descriptor() const;
    void destroy();
};
```

`VulkanUploadContext` agrupa `device`, `physical_device`, `queue`,
`command_pool` (pool de upload criado no `init()`), `descriptor_pool` e
`sampler_layout`. Para uso dentro da cena, chame `scene_.load_texture(...)`;
o contexto é montado automaticamente.

---

## 3. Shaders embutidos e regeneração

Os quatro sources GLSL estão no comentário de `vk.hpp` e são compilados durante
o build da cena para `VkShaderModule` a partir dos arrays de palavras SPIR-V:

| Pipeline | Vertex | Fragment | Uso |
|---|---|---|---|
| Geometria | `kGeometryVertSpv` | `kGeometryFragSpv` | `scene_.draw(...)` |
| Texto | `kTextVertSpv` | `kTextFragSpv` | `scene_.draw_text(...)` |

Bloco uniform (std140, 160 bytes) declarado nos dois vertex shaders e nos dois
fragment shaders:

```glsl
layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 u_projection;
    mat4 u_model;
    vec4 u_color;
    float u_use_texture;
} ubo;
layout(set = 1, binding = 0) uniform sampler2D u_texture;
```

| Campo C++ | Offset | Tamanho |
|---|---|---|
| `Mat4 projection` | 0 | 64 |
| `Mat4 model` | 64 | 64 |
| `float color[4]` | 128 | 16 |
| `float use_texture` | 144 | 4 (+ 12 de padding) |

O offset de cada item no UBO é `slot * uniform_stride_`, onde
`uniform_stride_` é arredondado para `minUniformBufferOffsetAlignment` (mínimo
observado no dispositivo). O descritor do UBO é `UNIFORM_BUFFER_DYNAMIC`, por
isso um único descriptor set por frame atende a até 64 itens.

**UVs:** os vertex shaders usam `v_uv = a_uv` e o upload preserva a ordem das
linhas do decode. Ou seja, os UVs de `MeshData::quad()` e do `FontAtlas` são
aplicados exatamente como no `GLES2Scene`, incluindo a convenção de orientação
já existente no backend GL (sem "correção" específica do Vulkan).

Para regenerar o SPIR-V (fora do apkfast; `glslangValidator` **não** é
dependência de build):

```sh
glslangValidator -V -S vert geometry.vert -o geometry.vert.spv
glslangValidator -V -S frag geometry.frag -o geometry.frag.spv
glslangValidator -V -S vert text.vert -o text.vert.spv
glslangValidator -V -S frag text.frag -o text.frag.spv
```

Depois converta cada `.spv` para `uint32_t` little-endian e substitua os arrays
no header. A fonte GLSL versionada no comentário é a referência; não edite
apenas o array.

---

## 4. Descritores, texturas e cache

### 4.1 Layouts

| Set | Binding | Tipo | Stage | Conteúdo |
|---|---|---|---|---|
| 0 | 0 | `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` | vertex + fragment | UBO do frame (projection/model/color/use_texture) |
| 1 | 0 | `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER` | fragment | textura branca, textura da material ou atlas de fonte |

O pool de descritores permite 2 sets de UBO e 64 sets de sampler. Cada
`TextureVk` recebe seu próprio descriptor set; o mesmo set é reutilizado em
todos os frames.

### 4.2 `TextureVk::load_rgba()`

1. cria `VkImage` `R8G8B8A8_UNORM`, `TILING_OPTIMAL`,
   `TRANSFER_DST | SAMPLED`, memória `DEVICE_LOCAL`;
2. cria staging buffer `HOST_VISIBLE | COHERENT`, copia as linhas respeitando
   `layout.rowPitch`;
3. em um command buffer one-time: transição `UNDEFINED → TRANSFER_DST_OPTIMAL`,
   `vkCmdCopyBufferToImage`, transição `TRANSFER_DST_OPTIMAL →
   SHADER_READ_ONLY_OPTIMAL`;
4. submete e chama `vkQueueWaitIdle()` (upload síncrono e seguro);
5. cria image view, sampler linear/clamp-to-edge e o descriptor set.

`destroy()` libera descriptor set, sampler, view, imagem e memória; é
idempotente. A textura branca 1×1 é criada no `init()` e usada quando
`Material::texture` está vazio (com `use_texture = 0`).

### 4.3 Cache por caminho

`draw()` com `Material::texture = "sprite.png"` chama `ensure_texture()`:
se o caminho ainda não está no cache, decodifica e sobe uma `TextureVk`.
Falhas são avisadas no logcat e caem para a textura branca sem `use_texture`,
exatamente como `GLES2Scene::draw_mesh()`.

---

## 5. `draw()` e variantes de pipeline

```cpp
void draw(const MeshData& mesh, const Material& material,
          const float* model = nullptr) override;
```

`draw()` **não desenha imediatamente**: copia a geometria, a matriz model
(identidade quando `model == nullptr`), a cor, o caminho da textura e as flags
`depth_test`/`blend` para uma fila de itens. O upload e a gravação dos comandos
acontecem em `render()`, depois que a fence do frame foi esperada. Isso evita
destruir buffers ainda em uso pelos 2 frames in-flight.

A variante de pipeline é escolhida por `(text, depth_test, blend, topology)`:

| Material | Pipeline |
|---|---|
| `depth_test = true`, `blend = true` (default) | depth on + blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA |
| `depth_test = false` | depth test/write off |
| `blend = false` | blend off (substitui o destino) |
| `draw_text()` | layout de vértice `x,y,u,v`, blend alpha, depth off |

Pipelines são criados sob demanda e reaproveitados; `invalidate()` os destrói
porque o render pass morre com a swapchain.

**Atenção — pipeline legado:** se você chamar `set_pipeline()` com SPIR-V
externo, esse pipeline continua sendo usado para geometria (API antiga mantida),
mas não há descritores dele: `Material` e `model` não são aplicados nesse modo.
Não chame `set_pipeline()` se quiser a paridade completa.

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

`load_font()` usa `FontAtlas::load()` (stb_truetype, ASCII imprimível 32–126),
expande o atlas 1 canal para RGBA com alpha no canal A e sobe como `TextureVk`.
`draw_text()` usa `FontAtlas::quad()` para avançar `x`/`y` e gera 6 vértices
por glyph com `vk_detail::GlyphVertex { x, y, u, v }`.

As coordenadas são as mesmas do GL: `resize(w,h)` (ou a janela inicial em
`attach()`) define `projection_ = vk_detail::ortho2d_screen(w, h)`, isto é,
origem no canto superior esquerdo e pixels como unidade. O NDC do Vulkan tem
Y invertido em relação ao GL, então a matriz usa `mat4_ortho(0, w, 0, h, -1, 1)`
em vez do `mat4_ortho2d(w, h)` do GL; o resultado em tela é idêntico.
`set_projection()` permite substituir essa matriz (nela, use o NDC do Vulkan).

---

## 7. Ciclo de vida e sincronização

API antiga preservada:

```cpp
void init(ANativeActivity* activity);
void init(ANativeActivity* activity, ANativeWindow* window);
void attach(ANativeWindow* window);
void invalidate();
void destroy();
void set_clear_color(float r, float g, float b, float a = 1.0f);
void set_present_mode(VkPresentModeKHR mode);
void set_validation(bool enabled);   // ainda sem layers reais
void resize(int, int);
void begin_frame() override;         // vazio; clear está no render pass
void present() override;             // alias de render()
void set_pipeline(PipelineConfig);
void render();
```

Sequência de um frame em `render()`:

1. espera a fence do frame (1 s) e lida com `DEVICE_LOST`;
2. `vkAcquireNextImageKHR` (recria swapchain em `OUT_OF_DATE`/`SUBOPTIMAL`);
3. espera a fence da imagem, se ela ainda estiver em uso;
4. `prepare_frame()`: descarta buffers do uso anterior, faz upload da geometria
   nova e grava os uniforms no UBO do frame;
5. grava clear + viewport + todos os itens no command buffer;
6. submit com semáforos `image_available → render_finished` e fence do frame;
7. `vkQueuePresentKHR` e `current_frame_++`.

Recursos de device (upload pool, layouts, descriptor pool, UBOs, textura
branca, cache de texturas e fonte) são criados em `init()` e destruídos em
`destroy()`. Recursos ligados ao render pass (pipelines) são recriados em
`attach()`/`recreate_swapchain()`.

---

## 8. Exemplo completo

`examples/nativeapp-vulkan/jni/main.cpp` demonstra clear color, triângulo
colorido, quad texturizado e texto. Estrutura essencial:

```cpp
void MeuApp::on_ready() {
    scene_.init(activity());
    scene_.set_clear_color(0.10f, 0.20f, 0.35f, 1.0f);
    has_sprite_ = scene_.load_texture("sprite.png");
    has_text_ = scene_.load_font("font.ttf", 28);
}

void MeuApp::on_surface_created() { scene_.attach(window()); }
void MeuApp::on_surface_destroyed() { scene_.invalidate(); }
void MeuApp::on_surface_changed(int w, int h) { scene_.resize(w, h); }

void MeuApp::on_draw() {
    const MeshData triangle = MeshData::triangle();
    Material colored;  // sem textura, depth/blend default
    const Mat4 model = mat4_multiply(
        mat4_translate(width() * 0.5f, height() * 0.35f, 0.0f),
        mat4_scale(160.0f, 160.0f, 1.0f));
    scene_.draw(triangle, colored, model.data());

    if (has_sprite_) {
        Material textured;
        textured.texture = "sprite.png";
        scene_.draw(MeshData::quad(180, 180), textured,
                    mat4_translate(width() * 0.7f, height() * 0.55f, 0.0f).data());
    }
    scene_.draw_text("VULKAN OK", 24, 48, 1.4f);
    scene_.render();
}
```

### Assets necessários

Assets são por APK; o exemplo **não** contém arquivos binários versionados.
Antes de buildar, coloque:

```text
examples/nativeapp-vulkan/assets/font.ttf    # qualquer TTF/OTF ASCII
examples/nativeapp-vulkan/assets/sprite.png  # PNG (ou .webp/.jpg/.svg ajustando o código)
```

Sem eles o app continua rodando: loga um aviso, desenha o triângulo colorido e
pula a textura/texto. Os assets de `examples/image-demo/assets/` e
`examples/media-demo/assets/` **não** são visíveis dentro deste APK.

### Build e verificação

```sh
python3 apkfast.py build examples/nativeapp-vulkan -o /tmp/vk-parity.apk -q
sh scripts/device-check.sh /tmp/vk-parity.apk com.apkfast.nativeappvk
python3 -m unittest discover -s tests -q
```

---

## 9. Tabela de paridade GL × VK

| Recurso | GLES2Scene | VulkanScene |
|---|---|---|
| Clear color | `glClearColor()` em `begin_frame()` | `VkClearValue` no render pass |
| Projeção | `mat4_ortho2d(w,h)` em `resize()` | `vk_detail::ortho2d_screen(w,h)` (NDC VK), em `resize()`/`attach()` |
| Geometria | `Mesh` com VBO/IBO | `GpuBuffer` com vertex/index buffer |
| `Material::color` | uniform `u_color` | UBO `SceneUniforms::color` |
| `Material::texture` | cache `std::unordered_map<string,Texture>` | cache `std::unordered_map<string,TextureVk>` |
| PNG/JPEG/WebP/SVG | `decode_asset_image()` | `decode_asset_image()` (mesmo decode) |
| Textura branca | não usada (sem textura → `vec4(1)`) | `TextureVk` 1×1 + `use_texture = 0` |
| Filtragem | linear + clamp | linear + clamp |
| `depth_test` | `glEnable/Disable(GL_DEPTH_TEST)` | pipeline com depth on/off |
| `blend` | `glEnable/Disable(GL_BLEND)` | pipeline com blend on/off |
| Matriz `model` | uniform `u_model` | UBO `SceneUniforms::model` |
| Vários desenhos/frame | uniforms por chamada | dynamic offsets no UBO + sets por textura |
| Textura externa | `draw_with_texture()` | `TextureVk`/`load_texture()` + `Material::texture` |
| Fonte | `FontAtlas` + `GL_ALPHA→RGBA` | `FontAtlas` + `TextureVk` RGBA |
| Quads de glyph | 6 vértices x,y,u,v | 6 vértices x,y,u,v |
| Pipeline de texto | blend alpha, sem depth | blend alpha, sem depth |
| Orientação UV | UVs GL aplicados como estão | `v_uv = a_uv`; upload sem espelhar linhas |
| Lifecycle | `init/resize/present/destroy` | `init/attach/invalidate/resize/render/destroy` |
| In-flight | EGL duplo buffer | 2 frames + fences + UBO por frame |

---

## 10. Limitações restantes

- `set_validation(true)` ainda não habilita layers de validação.
- Sem mipmaps/anisotropia, sem MSAA e sem cull configurável na API da cena
  (os pipelines embutidos usam `VK_CULL_MODE_NONE`, como o GL).
- Cache limitado a 64 texturas e 64 itens por frame; itens excedentes são
  ignorados com aviso no logcat.
- `FontAtlas` cobre apenas ASCII imprimível (32–126), igual ao GL.
- Upload de textura usa `vkQueueWaitIdle()`: é simples e seguro, mas síncrono.
- SVG respeita `svg_w`/`svg_h` apenas na nova sobrecarga de `load_texture()`;
  `Material::texture` usa o tamanho natural do documento.
- `draw_with_texture()` (GL) não tem equivalente direto; use uma `TextureVk`
  carregada por caminho via `Material::texture`.
- `GraphicsPipeline` direto com `text_layout = true` exige o layout de vértice
  `x,y,u,v`; o layout padrão continua sendo `Vertex` (12 floats).
- O exemplo não versiona assets; veja a seção 8.
