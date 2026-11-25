# Matriz de paridade OpenGL ES × Vulkan (`nativeapp/`)

> **Snapshot auditado:** apkfast 2.7.3, após a eliminação dos P0/P1 e dos P2
> G09–G11/G13–G15 no runtime. Este arquivo é o inventário vivo das diferenças
> que **ainda existem**, com o histórico dos gaps já fechados.
>
> **Fonte de verdade:** `include/nativeapp/gl.hpp` (`GLES2Scene`, `GLES3Scene`),
> `include/nativeapp/vk.hpp` (`VulkanScene`), `include/nativeapp/graphics_types.hpp`,
> `include/nativeapp/font.hpp`, `include/nativeapp/image_codecs.hpp`,
> `include/nativeapp/animated_image.hpp`, `include/nativeapp/animated_svg.hpp`,
> `docs/graphics-opengl.md`, `docs/graphics-vulkan.md`, `docs/image-formats.md` e
> os exemplos `examples/image-demo`, `examples/media-demo`,
> `examples/nativeapp-advanced`, `examples/nativeapp-gl`,
> `examples/nativeapp-vulkan`.
>
> A verificação automática desta matriz vive em `tests/test_parity_matrix.py`
> (estática: não precisa de GPU, SDK ou NDK). Os marcadores `[ambos]`,
> `[GL only]`, `[VK only]` e `[ausente]` na coluna de cada backend são
> verificados contra os símbolos reais dos headers; se o código mudar, a matriz
> e o teste mudam juntos. A compatibilidade visual é verificada por
> `tests/test_visual_parity.py` e `scripts/visual-parity.sh`.

## Critérios de classificação

| Classificação | Significado operacional |
|---|---|
| **BLOCKER VISUAL** | Falta pixel, texto ou frame esperado em um caminho suportado no outro backend; exige aprovação explícita antes de afirmar paridade. |
| **GAP DE API** | Método/símbolo existe só em um backend; o app precisa escrever código diferente para ter o mesmo efeito. |
| **GAP DE QUALIDADE** | O efeito final existe, mas diagnóstico, robustez, limites ou custo diferem. |
| **ACEITÁVEL COM JUSTIFICATIVA TÉCNICA** | Diferença de forma, não de resultado; documentada e fora do contrato comum (`Scene`/`Material`). |

---

## Matriz por capacidade

| ID | Capacidade | GLES2Scene / GLES3Scene | VulkanScene | Classificação | Evidência |
|---|---|---|---|---|---|
| C01 | lifecycle | `[GL only]` `init(ANativeWindow*)` idempotente, `GLContext` sobrevive à cena, `destroy()` virtual; `render()` legado faz clear+draw+present em NDC. | `[VK only]` `init(ANativeActivity*)` + `attach(window)` + `invalidate()`; device e swapchain têm vidas separadas; `render()`/`present()` com fila, `set_legacy_render()`/`render_legacy()` para NDC. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G15 resolvido, G18) | `gl.hpp:GLContext/GLES2Scene::render`; `vk.hpp:VulkanScene::init/attach/render_legacy` |
| C02 | clear | `[ambos]` `set_clear_color()` + `glClear(COLOR + DEPTH)` em `begin_frame()`; depth/blend/blendFunc restaurados. | `[ambos]` `set_clear_color()` vira `VkClearValue` no render pass; `begin_frame()` é vazio, clear em `render()`. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G19) | `gl.hpp:GLES2Scene::begin_frame`; `vk.hpp:VulkanScene::record_command_buffer` |
| C03 | geometria | `[GL only]` `Mesh` (VBO/IBO), `uint16`, 6 topologias, valida índice e cai para `drawArrays`, `set_vertex_count()`. | `[VK only]` `GpuBuffer` (vertex/index HOST_VISIBLE), `uint16`, 6 topologias, valida índice e cai para `drawArrays`, upload em `prepare_frame()`. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G06 resolvido) | `gl.hpp:Mesh::upload`; `vk.hpp:GpuBuffer::upload/create_buffer` |
| C04 | Material/cor | `[ambos]` `u_color = material.color` multiplica `v_color`; `depth_test`/`blend` por draw. | `[ambos]` `SceneUniforms::color` no UBO; variantes de pipeline por `depth_test`/`blend` (`PipelineKey`); pipeline custom também recebe SceneUniforms. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G02 resolvido; G16 em documentação) | `gl.hpp:draw_mesh`; `vk.hpp:draw/prepare_frame/get_or_create_pipeline` |
| C05 | textura | `[ambos]` `decode_asset_image()` (PNG/JPEG/WebP/SVG), RGBA, linear + clamp, 1 textura por draw; `Texture::bind(unit)` pública. | `[ambos]` mesmo decode, `TextureVk` DEVICE_LOCAL via staging, sampler linear/clamp, 1 descriptor set por textura; upload espelha linhas para convenção GL; `VulkanUploadContext` para texturas externas. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G01/G05 resolvidos) | `gl.hpp:Texture`; `vk.hpp:TextureVk/VulkanUploadContext` |
| C06 | SVG | `[ambos]` `load_svg_asset(path,w,h)` (NanoSVG/LunaSVG por magic bytes); `Material::texture` usa tamanho natural. | `[ambos]` `load_svg_asset(ctx,path,w,h)` e `load_texture(path,w,h)`; `Material::texture` usa tamanho natural. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G22) | `image_codecs.hpp:decode_svg_auto`; `gl.hpp:Texture::load_svg_asset`; `vk.hpp:TextureVk::load_svg_asset` |
| C07 | WebP animado | `[ambos]` `AnimatedImage` + `Texture::load_rgba()`/`TextureVk::load_rgba()` + `draw_with_texture()`; usado por `examples/image-demo`. | `[ambos]` decoder compartilhado; `draw_with_texture()` aceita `TextureVk` externa para frames de WebP/SMIL. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G01 resolvido) | `animated_image.hpp`; `image-formats.md:46-64`; `examples/image-demo/jni/main.cpp`; `vk.hpp:VulkanScene::draw_with_texture` |
| C08 | fonte TTF/draw_text | `[ambos]` `load_font()`/`has_font()`/`draw_text()` com `FontAtlas` (ASCII 32–126), atlas 1 canal expandido para RGBA, 6 vértices/glyph, blend alpha, sem depth. | `[ambos]` mesmos símbolos e mesmo `FontAtlas`; atlas vira `TextureVk`, glyphs viram `DrawItem` enfileirado; origem no topo, sem `height - y`. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G03/G08 resolvidos, G21) | `gl.hpp:768-854`; `vk.hpp:VulkanScene::draw_text`; `font.hpp:FontAtlas` |
| C09 | instancing | `[ambos]` `GLES3Scene::draw_instanced()` real: um `glDrawElementsInstanced`/`glDrawArraysInstanced` com VBO de `Instance` (matriz+cor) e divisor 1; assinatura antiga com `std::vector<Mat4>` preservada, fallback explícito em GLES2. | `[ambos]` `VulkanScene::draw_instanced()` com `std::vector<Mat4>`/`float*` e `upload_instances()`; um `vkCmdDrawIndexed`/`vkCmdDraw` com `instanceCount` e binding de instância. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G09 resolvido) | `gl.hpp:GLES3Scene::draw_instanced`; `vk.hpp:VulkanScene::draw_instanced/GpuBuffer::upload_instances` |
| C10 | depth | `[ambos]` `glEnable/Disable(GL_DEPTH_TEST)` por material; clear de depth no `begin_frame()`. | `[ambos]` `depthTestEnable/depthWriteEnable` no pipeline; anexo de depth sempre presente, `LESS`, clear 1.0. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA | `gl.hpp:draw_mesh`; `vk.hpp:GraphicsPipeline::build_with_layouts/create_depth_resources` |
| C11 | blend | `[ambos]` `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` global, ligado/desligado por material. | `[ambos]` mesmo par de fatores no `VkPipelineColorBlendAttachmentState`, por variante de pipeline. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA | `gl.hpp:init/begin_frame`; `vk.hpp:GraphicsPipeline` |
| C12 | cull | `[ausente]` cena nunca chama `glEnable(GL_CULL_FACE)`; sem parâmetro de cull na API alta. | `[VK only]` `PipelineConfig::cull_mode` existe (default `VK_CULL_MODE_NONE`), mas a cena não expõe cull. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G17) | `vk.hpp:PipelineConfig::cull_mode`; `gl.hpp:GLES2Scene` |
| C13 | projeção 2D/3D | `[ambos]` `resize()` instala ortho em pixels; `set_projection()` aceita qualquer `Mat4` (3D com `mat4_perspective_gl`, depth −1..1). | `[ambos]` `resize()` instala `vk_detail::ortho2d_screen()` (`mat4_ortho(0,w,0,h,-1,1)` no NDC VK); `set_projection()` aceita `Mat4` (3D com `mat4_perspective`, depth 0..1). | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G20) | `gl.hpp:resize`; `vk.hpp:resize/set_projection`; `math.hpp:mat4_perspective*` |
| C14 | uniforms/model | `[GL only]` `Shader::set_uniform*`/`set_uniform_mat4()` para shaders GLSL customizados via `set_shader()`; bloco `SceneUniforms` embutido nos shaders padrão. | `[VK only]` `SceneUniforms` no UBO + `register_custom_uniforms()`/`write_uniform()`/`bind_uniform_texture()` (set 2) e `set_pipeline(config, handle)` para SPIR-V de usuário. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G10/G11 resolvidos: GLSL em runtime vs SPIR-V embutido) | `gl.hpp:Shader::set_uniform`; `vk.hpp:CustomUniformBinding/register_custom_uniforms` |
| C15 | cache de textura | `[ambos]` `textures_` por caminho, sem limite; cache limpo em `destroy()`. | `[ambos]` `textures_` por caminho, sem limite rígido; pools crescem sob demanda; `invalidate_texture()`, `reload_texture()`, `clear_textures()` e `texture_count()`. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G12 resolvido) | `gl.hpp:textures_`; `vk.hpp:invalidate_texture/reload_texture/clear_textures/texture_count` |
| C16 | múltiplos draws por frame | `[ausente]` GL desenha imediatamente, sem teto na cena (alta de VBO por `draw()`). | `[VK only]` `kMaxDrawSlots = 64` é capacidade inicial do UBO; `grow_uniform_slots()` cresce sob demanda, nenhum `draw()` aceito é descartado. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G04 resolvido) | `gl.hpp:draw/draw_mesh`; `vk.hpp:prepare_frame/grow_uniform_slots` |
| C17 | resize | `[ambos]` `resize(w,h)` recalcula projeção e ignora dimensões <= 0; GL chama `glViewport`. | `[ambos]` `resize(w,h)` recalcula projeção, marca `projection_set_` e recria swapchain; usa `extent_` da swapchain no viewport. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G08 resolvido) | `gl.hpp:resize`; `vk.hpp:resize/recreate_swapchain` |
| C18 | device loss | `[ambos]` `context_lost()`/`device_lost()` detectam `EGL_CONTEXT_LOST`, expõem `needs_asset_reload()` e recriam EGL/shader/mesh/fonte via `recreate()`. | `[ambos]` `device_lost()` detecta `VK_ERROR_DEVICE_LOST`, expõe `needs_asset_reload()`, bloqueia `render()` e reinicia com `init()`. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G07 resolvido) | `gl.hpp:context_lost/needs_asset_reload/recreate`; `vk.hpp:handle_device_loss/device_lost` |
| C19 | validation layers | `[GL only]` `gl_check()` chama `glGetError()` somente com `APKFAST_DEBUG`; nunca lança. | `[VK only]` `vk_check()` lança `VulkanError`; `set_validation(true)` habilita `VK_LAYER_KHRONOS_validation` quando disponível, cria `VK_EXT_debug_utils` messenger, expõe `validation_enabled()`. | GAP DE QUALIDADE (G13 resolvido no VK; diagnóstico GL permanece por log) | `gl.hpp:48-58`; `vk.hpp:create_instance/on_validation_message/validation_enabled` |
| C20 | upload assíncrono | `[ausente]` `glBufferData`/`glTexImage2D` síncronos na thread; `temporary_mesh_` reenvia geometria em cada `draw()`. | `[VK only]` `VulkanUploadTracker`: staging com fence própria + semáforo `ready`, esperado pelo próximo submit de `render()`; sem `vkQueueWaitIdle()` no caminho da cena. | GAP DE QUALIDADE (G14 resolvido no VK; GL síncrono por API) | `gl.hpp:Mesh::upload/Texture::upload_rgba`; `vk.hpp:VulkanUploadTracker/track_upload/pending_semaphores` |
| C21 | API legada set_pipeline | `[GL only]` `set_shader(Shader*)` com GLSL compilado em runtime, uniforms nomeados e locations fixas; `render()` em NDC. | `[VK only]` `set_pipeline(PipelineConfig[, CustomUniformHandle])` com SPIR-V (embutido ou de usuário), `Material`/`model` aplicados; `render()`/`render_legacy()` em NDC. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G11/G15 resolvidos: linguagem de shader diferente) | `gl.hpp:set_shader/set_mesh/render`; `vk.hpp:set_pipeline/render_legacy` |
| C22 | performance/limites | `[ambos]` `MeshData::max_vertices = 65536` (`uint16`), `set_vertex_count()`, `draw_mesh()` reaproveita VBO; `GLES3Scene` usa instancing real. | `[ambos]` mesmo limite de `uint16`, UBO cresce sob demanda, cache de textura sem limite rígido, 2 frames in-flight, pipelines por combinação de estado, upload assíncrono de textura. | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA (G04/G06/G09/G12/G14 resolvidos) | `graphics_types.hpp:39-40`; `gl.hpp:GLES3Scene`; `vk.hpp:grow_uniform_slots/VulkanUploadTracker` |

> Nota de terminologia: a coluna "GL/VK" descreve quem **expõe o símbolo ou o
> mecanismo**, não quem o implementa melhor. `[ambos]` significa equivalência
> funcional no caminho embutido; as classificações apontam o gap restante.

---

## Plano de eliminação

Ordem: **P0** = bloqueia a afirmação de paridade; **P1** = risco de pixel/estado
faltando em apps normais; **P2** = paridade de API/qualidade; **P3** = otimização.
"Arquivo dono" é onde a mudança de runtime foi/será feita. `RESOLVIDO` significa
que o símbolo e o comportamento foram conferidos no header atual; linhas sem
`RESOLVIDO` na coluna de gap são pendências reais.

| ID | Gap | Capacidade | Classificação | Ordem | Arquivo dono (símbolo) | Risco | Critério de pronto |
|---|---|---|---|---|---|---|---|
| G01 | RESOLVIDO — `draw_with_texture()` em VK com `upload_context()`/`TextureVk::load_rgba()`; frames WebP/SMIL têm caminho de display. | C07 | BLOCKER VISUAL | — | `include/nativeapp/vk.hpp` (`VulkanScene::draw_with_texture`) | nenhum (documentado) | RESOLVIDO: API pública, decode compartilhado e lifetime preso ao frame via fence/semáforo. |
| G02 | RESOLVIDO — texto no modo legado usa sempre pipeline de texto embutido com UBO/atlas; geometria externa recebe SceneUniforms. | C04, C21 | BLOCKER VISUAL | — | `include/nativeapp/vk.hpp` (`record_command_buffer`) | nenhum (documentado) | RESOLVIDO: sem layout de `Vertex` em item de texto e sem descarte de `Material`/`model`. |
| G03 | RESOLVIDO — pool de samplers dimensionado por `kMaxTextures + kTextureDescriptorReserve` e sets de UBO com `kUboSetReserve`. | C08, C15 | BLOCKER VISUAL | — | `include/nativeapp/vk.hpp` (`create_shared_gpu_resources`) | nenhum (documentado) | RESOLVIDO: reserva cobre white + fonte + texturas externas com cache cheio. |
| G04 | RESOLVIDO — `grow_uniform_slots()` faz o UBO crescer; `kMaxDrawSlots` é só capacidade inicial. | C16, C22 | BLOCKER VISUAL | — | `include/nativeapp/vk.hpp` (`prepare_frame`) | nenhum (documentado) | RESOLVIDO: nenhum `draw()` aceito é descartado por teto fixo. |
| G05 | RESOLVIDO — `TextureVk::load_rgba()` espelha linhas e usa staging contíguo; `draw()` não inverte `v` globalmente. | C05 | GAP DE QUALIDADE | — | `include/nativeapp/vk.hpp` (`TextureVk::load_rgba`) | nenhum (documentado) | RESOLVIDO: UVs GL, quad e atlas com a mesma orientação; verificação visual em `docs/visual-parity.md`. |
| G06 | RESOLVIDO — `GpuBuffer::upload()` valida índice `uint16` e cai para `drawArrays` com aviso, como no GL. | C03, C22 | GAP DE QUALIDADE | — | `include/nativeapp/vk.hpp` (`GpuBuffer::upload`) | nenhum (documentado) | RESOLVIDO: índice inválido nunca chega ao `vkCmdDrawIndexed`. |
| G07 | RESOLVIDO — GL detecta `EGL_CONTEXT_LOST`, expõe `context_lost()`/`device_lost()`/`needs_asset_reload()` e recria EGL/shader/mesh/fonte com `recreate()`; VK usa `device_lost()`/`needs_asset_reload()`/`init()`. | C18 | GAP DE QUALIDADE | — | `include/nativeapp/gl.hpp` (`GLContext`, `GLES2Scene`) + `include/nativeapp/vk.hpp` (`handle_device_loss`) | nenhum (documentado) | RESOLVIDO: contrato de recarga equivalente nos dois. Injeção controlada de perda real não disponível no emulador; validado por código/build/execução normal. |
| G08 | RESOLVIDO — `draw_text()` usa `y` direto em `FontAtlas::quad()`; projeção customizada e padrão produzem a mesma posição. | C08, C17 | GAP DE QUALIDADE | — | `include/nativeapp/vk.hpp` (`VulkanScene::draw_text`) | nenhum (documentado) | RESOLVIDO: sem `extent_.height - y`; caixas de texto iguais a `GLES2Scene`. |
| G09 | RESOLVIDO — instancing real nos dois: `glDraw*Instanced` com divisor 1 e `vkCmdDraw*` com `instanceCount`/binding de instância; assinatura `std::vector<Mat4>` preservada. | C09, C22 | GAP DE API + GAP DE QUALIDADE | — | `include/nativeapp/vk.hpp` (`VulkanScene::draw_instanced`) + `include/nativeapp/gl.hpp` (`GLES3Scene`) | nenhum (documentado) | RESOLVIDO: `draw_instanced(` existe nos dois headers e exemplos; uma chamada de draw por lote. |
| G10 | RESOLVIDO — `register_custom_uniforms()`/`write_uniform()`/`bind_uniform_texture()` com descritor no set 2; GL mantém `set_uniform*` nomeado. | C14 | GAP DE API | — | `include/nativeapp/vk.hpp` (`CustomUniformBinding`) | nenhum (documentado) | RESOLVIDO: uniforms arbitrários e textura de usuário disponíveis em ambos, com handle por binding no VK. |
| G11 | RESOLVIDO — `set_pipeline(config, handle)` aplica `SceneUniforms`/`Material`/`model` também no pipeline custom; GL usa GLSL em runtime. | C21 | GAP DE API | — | `include/nativeapp/vk.hpp` (`VulkanScene::set_pipeline`) + `include/nativeapp/gl.hpp` (`set_shader`) | nenhum (documentado) | RESOLVIDO: semântica de shader de usuário equivalente; linguagem (SPIR-V vs GLSL) documentada como diferença de forma. |
| G12 | RESOLVIDO — cache sem limite rígido nos dois; VK adiciona `invalidate_texture()`, `reload_texture()`, `clear_textures()` e `texture_count()`; pools de descritores crescem sob demanda. | C15, C22 | GAP DE QUALIDADE | — | `include/nativeapp/vk.hpp` (`ensure_texture`) + `include/nativeapp/gl.hpp` (`textures_`) | nenhum (documentado) | RESOLVIDO: nenhuma textura some por teto fixo e há invalidação explícita. |
| G13 | RESOLVIDO — `set_validation(true)` habilita `VK_LAYER_KHRONOS_validation` quando disponível, cria messenger `VK_EXT_debug_utils` e expõe `validation_enabled()`; GL continua com `gl_check()` só em debug. | C19 | GAP DE QUALIDADE | — | `include/nativeapp/vk.hpp` (`create_instance`) + `include/nativeapp/gl.hpp` (`gl_check`) | nenhum (documentado) | RESOLVIDO no VK: layers reais + callback de erro; em GL não há camadas, apenas log — diferença de diagnóstico documentada. |
| G14 | RESOLVIDO — `VulkanUploadTracker` usa fence própria e semáforo `ready` esperado no submit de `render()`; sem `vkQueueWaitIdle()` na cena. GL permanece síncrono por natureza da API. | C20, C22 | GAP DE QUALIDADE | — | `include/nativeapp/vk.hpp` (`track_upload`, `pending_semaphores`) | nenhum (documentado) | RESOLVIDO no VK: troca de frames de textura animada sem stall de fila; geometria estática reaproveitada. |
| G15 | RESOLVIDO — `set_legacy_render()`/`render_legacy()` e `render()` com projeção identidade desenham a malha em NDC e ignoram itens enfileirados, como `GLES2Scene::render()`. | C01, C21 | GAP DE API | — | `include/nativeapp/vk.hpp` (`VulkanScene::render`) | nenhum (documentado) | RESOLVIDO: `render()` legado existe nos dois e `present()` continua como alias no VK. |
| G16 | RESOLVIDO — `docs/graphics-types.md` §8 e `docs/graphics-vulkan.md` refletem o runtime atual: `Material`/`model` aplicados, validation layers reais, upload assíncrono, instancing, uniforms custom e render legado. | C04, C05, C09, C13, C19, C20 | GAP DE QUALIDADE | — | `docs/graphics-types.md`, `docs/graphics-vulkan.md` | nenhum (documentado) | RESOLVIDO: documentação reconciliada com headers e testes. |
| G17 | Sem cull, mipmap, anisotropia, MSAA ou stencil na API alta dos dois backends (VK expõe `cull_mode` apenas em `PipelineConfig`). | C12 | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA | — | `docs/parity-matrix.md` (documentação) | nenhum (documentado) | Nenhuma mudança de runtime; fora do contrato `Scene`/`Material` da base. |
| G18 | `init()` do GL recebe `ANativeWindow*`; o do VK recebe `ANativeActivity*` e depois `attach(window)`. Troca de backend não é literalmente drop-in. | C01 | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA | — | `docs/parity-matrix.md` (documentação) | nenhum (documentado) | Justificativa: Vulkan precisa da Activity para `VK_KHR_android_surface` e para injetar o `AAssetManager`; `init(activity, window)` existe como atalho. |
| G19 | `begin_frame()` limpa no GL e é vazio no VK; clear do VK está acoplado ao render pass. Alfa do clear e config ES3 fallback são diferentes (ES3 sem alpha 8, VK `OPAQUE`). | C02 | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA | — | `docs/parity-matrix.md` (documentação) | nenhum (documentado) | Fluxo `begin_frame()/draw()/present()` funciona nos dois; não usar o clear como operação isolada. |
| G20 | 3D exige `mat4_perspective_gl()` no GL (−1..1) e `mat4_perspective()` no VK (0..1); ambos estão em `math.hpp`. | C13 | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA | — | `docs/parity-matrix.md` (documentação) | nenhum (documentado) | Escolha do helper é responsabilidade do app; `set_projection()` aceita ambos em ambos. |
| G21 | `FontAtlas` cobre apenas ASCII imprimível (32–126), atlas fixo 512; sem kerning/RTL/ligaduras nos dois. | C08 | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA | — | `docs/parity-matrix.md` (documentação) | nenhum (documentado) | Mesma limitação em `font.hpp`, logo mesma saída nos dois backends. |
| G22 | SVG estático: GL `load_svg_asset(path,w,h)` vs VK `load_svg_asset(ctx,path,w,h)`/`load_texture(path,w,h)`; decode e rasterização são compartilhados. | C06 | ACEITÁVEL COM JUSTIFICATIVA TÉCNICA | — | `docs/parity-matrix.md` (documentação) | nenhum (documentado) | `Material::texture` usa tamanho natural nos dois; `ctx` no VK é exigido pelo upload de imagem. |

---

## Estado para aprovação

- **Gaps funcionais G01–G16: RESOLVIDOS.**
- **G17–G22:** diferenças documentadas e equivalentes nos dois backends (cull/MSAA, assinaturas de `init`, clear, 3D, ASCII, SVG `ctx`); não bloqueiam o uso normal.
- `tests/test_parity_matrix.py` exige os símbolos dos gaps RESOLVIDO nos headers; se algum desaparecer, a matriz volta a falhar.
- A validação visual final está em `docs/visual-parity.md` (GL e VK idênticos, máx. 1/255 por canal).

## O que pode ficar como "documentado" sem virar bug

G17–G22, mais as diferenças de forma já resolvidas: G10/G11 (GLSL vs SPIR-V),
G13 (layers no VK vs log no GL), G14 (upload assíncrono no VK vs síncrono no GL)
e G18/G19/G20/G22 (assinaturas e convenções de projeção). Elas devem continuar
nesta matriz para que "sem limitações" não seja afirmado de forma imprecisa,
mas não bloqueiam o uso normal dos backends.

## Divergências de documento detectadas nesta auditoria

Não foram editadas (fora da propriedade da Equipe Matriz), mas precisam de
correção pelo dono de cada arquivo — ver G16:

- `docs/graphics-types.md` §8: diz que `VulkanScene::draw()` ignora `material` e
  `model`; o header aplica ambos via UBO também no pipeline custom.
- `docs/graphics-vulkan.md` §2/§4.2/§7/§10: ainda diz que `set_validation(true)`
  não habilita layers e que `load_rgba()` usa `vkQueueWaitIdle()`; o header já
  tem `VK_LAYER_KHRONOS_validation`, messenger `VK_EXT_debug_utils` e
  `VulkanUploadTracker` com fence/semáforo.
- `docs/graphics-vulkan.md`: não documenta `draw_instanced()`,
  `register_custom_uniforms()`/`write_uniform()`/`bind_uniform_texture()` nem
  `set_legacy_render()`/`render_legacy()` introduzidos nesta rodada.
- `docs/graphics-opengl.md`: atualizado nesta rodada (instancing real, assets,
  limitações) — sem divergência encontrada.
