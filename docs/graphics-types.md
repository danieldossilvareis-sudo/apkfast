# Tipos gráficos comuns (`graphics_types.hpp`)

> Guia completo dos tipos compartilhados entre os backends GLES e Vulkan.
>
> **Fontes consultadas:** `include/nativeapp/graphics_types.hpp` (todos os
> tipos), `include/nativeapp/gl.hpp` (`GLES2Scene`, `GLES3Scene`,
> `Texture::load`), `include/nativeapp/vk.hpp` (`VulkanScene`) e
> `include/nativeapp/math.hpp` (`Mat4`, usado por `Scene::draw`).
>
> **Regra deste documento:** só está aqui o que existe no código atual. Onde o
> comportamento difere entre backends, isso é mostrado explicitamente (§8).

## Índice

1. [Visão geral](#1-visão-geral)
2. [`Vertex` — vértice padrão 3D](#2-vertex--vértice-padrão-3d)
3. [`SpriteVertex` — vértice 2D para sprites](#3-spritevertex--vértice-2d-para-sprites)
4. [`Topology` — seis modos de primitiva](#4-topology--seis-modos-de-primitiva)
5. [`MeshData` — geometria CPU-side](#5-meshdata--geometria-cpu-side)
6. [`Material` — cor, textura e estado](#6-material--cor-textura-e-estado)
7. [Interface `Scene`](#7-interface-scene)
8. [Implementações de `Scene`](#8-implementações-de-scene)
9. [Exemplo completo](#9-exemplo-completo)
10. [Armadilhas](#10-armadilhas)
11. [Resumo rápido](#11-resumo-rápido)

---

## 1. Visão geral

O header agrupa os **dados** que a aplicação monta na CPU e entrega a um
backend de renderização: vértices, índices, topologia, material e a interface
polimórfica `Scene`. Ele inclui `<cstdint>`, `<string>` e `<vector>` e define
tudo em `namespace nativeapp`.

```
  Vertex / SpriteVertex  ──►  MeshData (vertices + indices + topology)
                                      │
  Material (cor + texture + flags) ───┤
  Mat4 model (column-major) ──────────┤
                                      ▼
                              Scene::draw(...)
                                      │
                    ┌─────────────────┴──────────────────┐
                    ▼                                    ▼
             GLES2Scene / GLES3Scene                VulkanScene
```

Não há upload de GPU, shader, textura ou câmera neste header — isso pertence
aos backends. O comentário do próprio código resume: `MeshData` é geometria
reutilizável "CPU-side" e "o backend (GL/VK) faz o upload".

---

## 2. `Vertex` — vértice padrão 3D

```cpp
struct Vertex {
    float x = 0, y = 0, z = 0;
    float nx = 0, ny = 0, nz = 1;
    float u = 0, v = 0;
    float r = 1, g = 1, b = 1, a = 1;
};
```

### Campos, padrões e offsets

Todos os membros são `float` (4 bytes) e estão em sequência, sem padding entre
eles. Offsets em bytes a partir do início da estrutura:

| Offset | Campo | Tipo | Padrão | Uso |
|---:|---|---|---|---|
| 0 | `x` | `float` | `0` | Posição X |
| 4 | `y` | `float` | `0` | Posição Y |
| 8 | `z` | `float` | `0` | Posição Z |
| 12 | `nx` | `float` | `0` | Normal X |
| 16 | `ny` | `float` | `0` | Normal Y |
| 20 | `nz` | `float` | `1` | Normal Z (padrão apontando para +Z) |
| 24 | `u` | `float` | `0` | Coordenada de textura U |
| 28 | `v` | `float` | `0` | Coordenada de textura V |
| 32 | `r` | `float` | `1` | Cor RGBA, componente vermelha |
| 36 | `g` | `float` | `1` | Componente verde |
| 40 | `b` | `float` | `1` | Componente azul |
| 44 | `a` | `float` | `1` | Alfa |

Tamanho total: **12 floats = 48 bytes**.

O shader padrão de `GLES2Scene` declara exatamente esses quatro atributos,
na mesma ordem:

```glsl
attribute vec3 a_position;  // x, y, z       → offset 0
attribute vec3 a_normal;    // nx, ny, nz    → offset 12
attribute vec2 a_uv;        // u, v          → offset 24
attribute vec4 a_color;     // r, g, b, a    → offset 32
```

### Inicialização

`Vertex` é uma struct agregada: a inicialização com chaves segue a **ordem de
declaração dos campos**, não os nomes. É assim que `MeshData::quad()` monta
seus vértices:

```cpp
Vertex{-hw,  hh, 0,   0,0,1,   0,0,   1,1,1,1};
//     x    y  z   nx ny nz   u v    r g b a
```

Para usar valores padrão parciais, construa o objeto e atribua campos
individualmente:

```cpp
nativeapp::Vertex v;        // normal (0,0,1), UV (0,0), cor branca opaca
v.x = 1.0f;
v.u = 0.5f;
```

Cuidado: `Vertex{1.0f}` preenche apenas `x`; os demais campos usam os valores
padrão dos inicializadores de membro (não são todos zero — `nz`, `r`, `g`, `b`
e `a` começam em `1`).

---

## 3. `SpriteVertex` — vértice 2D para sprites

```cpp
struct SpriteVertex {
    float x = 0, y = 0;
    float u = 0, v = 0;
    float r = 1, g = 1, b = 1, a = 1;
};
```

| Offset | Campo | Tipo | Padrão |
|---:|---|---|---|
| 0 | `x` | `float` | `0` |
| 4 | `y` | `float` | `0` |
| 8 | `u` | `float` | `0` |
| 12 | `v` | `float` | `0` |
| 16 | `r` | `float` | `1` |
| 20 | `g` | `float` | `1` |
| 24 | `b` | `float` | `1` |
| 28 | `a` | `float` | `1` |

Tamanho total: **8 floats = 32 bytes**. É uma versão 2D do `Vertex`: sem
posição Z e sem normal.

Ponto importante: **`MeshData` usa `std::vector<Vertex>`, não
`SpriteVertex`**. O header não define funções, conversões ou helpers para
transformar `SpriteVertex` em `Vertex` (nem um `MeshData` alternativo para
sprites). Se você vai desenhar via `Scene::draw`, precisa de `MeshData` com
`Vertex`.

---

## 4. `Topology` — seis modos de primitiva

```cpp
enum class Topology {
    Triangles,
    TriangleStrip,
    TriangleFan,
    Lines,
    LineStrip,
    Points,
};
```

| Valor | Primitiva | Indice ordinal (implícito) |
|---|---|---:|
| `Topology::Triangles` | Triângulos independentes: grupos de 3 índices/vértices | 0 |
| `Topology::TriangleStrip` | Faixa de triângulos compartilhando vértices | 1 |
| `Topology::TriangleFan` | Leque de triângulos com vértice central comum | 2 |
| `Topology::Lines` | Segmentos de reta independentes: pares de índices/vértices | 3 |
| `Topology::LineStrip` | Polilinha contínua | 4 |
| `Topology::Points` | Pontos independentes | 5 |

É `enum class` (escopo fechado): use `Topology::Triangles`, nunca apenas
`Triangles`. `MeshData::topology` começa em `Topology::Triangles`, e o header
não faz validação de compatibilidade entre topologia e lista de índices —
isso é responsabilidade de quem monta a malha e do backend.

---

## 5. `MeshData` — geometria CPU-side

```cpp
struct MeshData {
    /// Último índice uint16_t é 65535; 65536 é a contagem máxima de vértices.
    static constexpr std::size_t max_vertices = 65536;

    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    Topology topology = Topology::Triangles;
    // ... add_triangle, add_quad, quad, triangle ...
};
```

### Campos

| Campo | Tipo | Padrão | Papel |
|---|---|---|---|
| `max_vertices` | `static constexpr std::size_t` | `65536` | Limite de vértices por malha (maior índice `uint16_t` = 65535). |
| `vertices` | `std::vector<Vertex>` | vazio | Lista de vértices; a posição na lista é o índice do vértice. |
| `indices` | `std::vector<uint16_t>` | vazio | Lista de índices (16 bits) que referenciam `vertices`. |
| `topology` | `Topology` | `Topology::Triangles` | Como os índices devem ser interpretados. |

A struct é um agregado simples: você também pode preencher `vertices`,
`indices` e `topology` diretamente, sem usar os helpers.

### `add_triangle(a, b, c)`

```cpp
void add_triangle(const Vertex& a, const Vertex& b, const Vertex& c) {
    if (vertices.size() > max_vertices - 3) {
        throw std::runtime_error("Mesh excede 65536 vertices (limite uint16)");
    }
    const uint16_t base = static_cast<uint16_t>(vertices.size());
    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(c);
    indices.push_back(base);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
}
```

| Aspecto | Comportamento |
|---|---|
| Vértices adicionados | 3 (`a`, `b`, `c`), nessa ordem, no fim de `vertices`. |
| Índices adicionados | 3: `base`, `base+1`, `base+2`. |
| `base` | Capturado **antes** dos `push_back`, como `uint16_t`. |
| Capacidade | Verificada **antes** de qualquer `push_back`: `vertices.size() > max_vertices - 3` lança `std::runtime_error`. |
| Topologia | Não é alterada; permanece o valor atual. |
| Junção com malha existente | Funciona por append: novos índices apontam para os novos vértices. |

**Sobre o limite de 16 bits (65536):** `max_vertices = 65536` porque o maior
índice `uint16_t` é 65535. O guarda explícito impede estouro e indexação
circular: uma malha com até 65533 vértices aceita um triângulo (chega a 65536);
com 65534 ou mais, `add_triangle()` lança `std::runtime_error` **sem alterar
`vertices` nem `indices`** (a checagem vem antes de todo `push_back`). Para
`add_quad()` o limite de entrada é 65530, pois ele adiciona 6 de uma vez.

### `add_quad(tl, tr, br, bl)`

```cpp
void add_quad(const Vertex& tl, const Vertex& tr,
              const Vertex& br, const Vertex& bl) {
    if (vertices.size() > max_vertices - 6) {
        throw std::runtime_error("Mesh excede 65536 vertices (limite uint16)");
    }
    add_triangle(tl, tr, br);
    add_triangle(tl, br, bl);
}
```

Espera os quatro cantos na ordem **top-left, top-right, bottom-right,
bottom-left**. Cada triângulo tem três vértices próprios (não há compartilhamento
de vértice entre eles), então o quad produz **6 vértices e 6 índices**:

```
triângulo 1: tl, tr, br  →  vértices base+0, base+1, base+2; índices base+0, base+1, base+2
triângulo 2: tl, br, bl  →  vértices base+3, base+4, base+5; índices base+3, base+4, base+5
```

A diagonal compartilhada é `tl → br` (os cantos aparecem duplicados, mas os
índices dos dois triângulos são disjuntos). A capacidade é checada **antes** de
qualquer `push_back`: um quad que não cabe lança `std::runtime_error` e deixa a
malha **exatamente como estava** — nunca uma mutação parcial (um triângulo
dentro e outro fora).

### `static MeshData quad(float w = 1.0f, float h = 1.0f)`

Retorna uma malha nova (por valor) com um retângulo centrado na origem,
`hw = w/2`, `hh = h/2`, normal `(0,0,1)`, cor branca opaca e topologia padrão:

| Índice | Posição `(x, y, z)` | UV `(u, v)` | Cor |
|---:|---|---|---|
| 0 (tl) | `(-hw,  hh, 0)` | `(0, 0)` | `(1,1,1,1)` |
| 1 (tr) | `( hw,  hh, 0)` | `(1, 0)` | `(1,1,1,1)` |
| 2 (br) | `( hw, -hh, 0)` | `(1, 1)` | `(1,1,1,1)` |
| 3 (tl) | `(-hw,  hh, 0)` | `(0, 0)` | `(1,1,1,1)` |
| 4 (br) | `( hw, -hh, 0)` | `(1, 1)` | `(1,1,1,1)` |
| 5 (bl) | `(-hw, -hh, 0)` | `(0, 1)` | `(1,1,1,1)` |

Índices: `{0, 1, 2, 3, 4, 5}`.

```cpp
auto q = nativeapp::MeshData::quad();          // 1×1, de -0.5 a 0.5
auto larga = nativeapp::MeshData::quad(200.0f, 100.0f); // -100..100, -50..50
```

### `static MeshData triangle()`

Retorna uma malha nova com três vértices, normal `(0,0,1)` e índices
`{0, 1, 2}`:

| Índice | Posição | UV | Cor RGBA |
|---:|---|---|---|
| 0 | `( 0.0,  0.5, 0)` | `(0.5, 1)` | `(1.0, 0.3, 0.3, 1)` |
| 1 | `(-0.5, -0.5, 0)` | `(0.0, 0)` | `(0.3, 1.0, 0.3, 1)` |
| 2 | `( 0.5, -0.5, 0)` | `(1.0, 0)` | `(0.3, 0.3, 1.0, 1)` |

É a malha padrão usada por `GLES2Scene::default_mesh()` e pelo método de
compatibilidade `render()`.

### Montando uma malha manualmente

```cpp
#include <nativeapp/graphics_types.hpp>

using namespace nativeapp;

MeshData m;
m.topology = Topology::Triangles;

Vertex v;
v.z = 0.0f;                 // nx=0, ny=0, nz=1, UV=(0,0), cor branca
v.x = -0.5f; v.y =  0.5f; v.u = 0.0f; v.v = 0.0f; const Vertex tl = v;
v.x =  0.5f; v.y =  0.5f; v.u = 1.0f; v.v = 0.0f; const Vertex tr = v;
v.x =  0.5f; v.y = -0.5f; v.u = 1.0f; v.v = 1.0f; const Vertex br = v;
v.x = -0.5f; v.y = -0.5f; v.u = 0.0f; v.v = 1.0f; const Vertex bl = v;

m.add_quad(tl, tr, br, bl);  // 6 vértices (tl,tr,br,tl,br,bl), 6 índices
```

---

## 6. `Material` — cor, textura e estado

```cpp
struct Material {
    float color[4] = {1, 1, 1, 1};
    std::string texture;
    bool depth_test = true;
    bool blend = true;
};
```

| Campo | Tipo | Padrão | Significado |
|---|---|---|---|
| `color` | `float[4]` | `{1, 1, 1, 1}` | Multiplicador RGBA enviado como uniform `u_color` no shader GLES padrão. |
| `texture` | `std::string` | vazia | Nome do arquivo em `assets/`; vazio significa "sem textura". |
| `depth_test` | `bool` | `true` | Liga/desliga `GL_DEPTH_TEST` em `GLES2Scene::draw_mesh()`. |
| `blend` | `bool` | `true` | Liga/desliga `GL_BLEND` em `GLES2Scene::draw_mesh()`. |

`texture` é exatamente o caminho relativo usado por `Texture::load()`: em
`GLES2Scene::draw_mesh()`, se a string não estiver vazia, o backend procura a
textura no mapa interno e, na primeira vez, chama `Texture::load(material.texture)`;
se o carregamento falhar, o uniform `u_use_texture` volta para `0` e é emitido
um aviso no log.

```cpp
nativeapp::Material mat;
// padrão: branco opaco, sem textura, depth_test=true, blend=true

mat.color[0] = 0.2f;             // vermelho
mat.color[3] = 0.5f;             // alfa
mat.texture  = "textures/grade.png";
mat.depth_test = false;          // útil para sobreposições 2D
mat.blend = true;
```

O header não inclui shader, culling, blend equation/func, bring depth write ou
outros estados — apenas os quatro campos acima.

---

## 7. Interface `Scene`

```cpp
class Scene {
public:
    virtual ~Scene() = default;
    virtual void resize(int width, int height) = 0;
    virtual void begin_frame() = 0;
    virtual void present() = 0;
    virtual void set_clear_color(float r, float g, float b, float a = 1.0f) = 0;

    /// Desenha uma mesh com material e matriz model (16 floats column-major).
    virtual void draw(const MeshData&, const Material&, const float* model = nullptr) = 0;
};
```

| Método | Parâmetros | Contrato |
|---|---|---|
| `~Scene()` | — | Destrutor virtual `= default`: permite destruir via ponteiro de `Scene`. |
| `resize(int, int)` | `width`, `height` | Notifica mudança de tamanho da superfície. |
| `begin_frame()` | nenhum | Inicia o quadro. |
| `present()` | nenhum | Encerra/submete o quadro. |
| `set_clear_color(float, float, float, float)` | `r`, `g`, `b`, `a = 1.0f` | Define a cor de limpeza. |
| `draw(const MeshData&, const Material&, const float*)` | malha, material, `model = nullptr` | Desenha a malha; `model` é um ponteiro para **16 floats column-major** (por exemplo, `Mat4::data()` de `math.hpp`). |

Todos os métodos são **puramente virtuais** (`= 0`): `Scene` não pode ser
instanciada diretamente. O ponteiro `model` não é copiado pela interface — ele
deve permanecer válido durante a chamada.

Ciclo de uso pretendido:

```cpp
scene->resize(largura, altura);
scene->set_clear_color(0.08f, 0.08f, 0.12f, 1.0f);
scene->begin_frame();
scene->draw(mesh, material, model.data());   // ou nullptr
scene->present();
```

A interface **não** declara `init`, `destroy`, `set_shader`,
`set_projection` ou `set_pipeline`: esses métodos existem apenas nas classes
concretas (veja §8). Por isso, a inicialização precisa ser feita pelo tipo
concreto antes de guardar o ponteiro como `Scene*`.

---

## 8. Implementações de `Scene`

| Classe | Header | Observações |
|---|---|---|
| `GLES2Scene : public Scene` | `gl.hpp` | Implementa todos os métodos virtuais; usa EGL + GLES2 e uma projeção `mat4_ortho2d(largura, altura)` instalada em `resize()`. |
| `GLES3Scene : public GLES2Scene` | `gl.hpp` | Herda a implementação de `Scene` (adicionalmente expõe `init(window, version)` com `using`). |
| `VulkanScene : public Scene` | `vk.hpp` | Implementa os virtuais com Vulkan; `init(activity)` também injeta o `AAssetManager` via `Assets::instance(activity).set_activity(activity)`. |

Comportamentos concretos que valem conhecer (todos lidos das implementações):

| Método | `GLES2Scene` | `VulkanScene` |
|---|---|---|
| `resize(w, h)` | Guarda `width_`/`height_`, chama `glViewport` e recalcula a projeção com `mat4_ortho2d(w, h)`. | Recria a swapchain se inicializada e com janela (ignora os parâmetros). |
| `begin_frame()` | Ativa o contexto EGL, restaura `GL_DEPTH_TEST`/`GL_BLEND`, define `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`, `glClearColor` e limpa cor + profundidade. | Corpo vazio (`// clear acontece no render pass`). |
| `present()` | Ativa EGL e chama `eglSwapBuffers` (com aviso no log se falhar). | Chama `render()`. |
| `set_clear_color(...)` | Guarda `{r,g,b,a}` em `clear_`. | Guarda `{r,g,b,a}` em `clear_`. |
| Device loss | `context_lost()`/`device_lost()` viram `true` em `EGL_CONTEXT_LOST`; `needs_asset_reload()` também. `init()`/`recreate()` recupera. | `device_lost()`/`needs_asset_reload()` viram `true` em `VK_ERROR_DEVICE_LOST`; `init()` recupera. |
| `draw(mesh, material, model)` | Faz upload temporário da malha e desenha: aplica material, textura, `depth_test`, `blend` e a matriz `model` (usa identidade quando `model == nullptr`). | Requer `set_pipeline()`; faz upload da malha para um `GpuBuffer` e enfileira `DrawItem` com `material.color`, `material.texture`, `material.depth_test`, `material.blend` e `model` (`nullptr` ⇒ identidade). Os valores são enviados ao UBO em `prepare_frame()`. |

Os dois backends agora têm o mesmo contrato de perda: `device_lost()` (no GL,
`context_lost()` é o nome primário e `device_lost()` um alias),
`needs_asset_reload()` e inicialização explícita para voltar a desenhar. A
diferença é o que cada um descarta: o GL usa `gl_detail::context_generation()`
para invalidar `Shader`/`Mesh`/`Texture` sem chamar `glDelete*` sobre nomes de
um contexto morto; o VK descarta handles de device com
`release_without_vulkan()`. Em ambos, recursos externos precisam ser
reenviados pelo app depois da recuperação.

Exemplo de uso polimórfico (a inicialização é feita no tipo concreto):

```cpp
#include <nativeapp/gl.hpp>
#include <memory>

std::unique_ptr<nativeapp::Scene> scene = std::make_unique<nativeapp::GLES2Scene>();
auto* gles = static_cast<nativeapp::GLES2Scene*>(scene.get());
gles->init(janela);                 // init() não faz parte de Scene

scene->resize(800, 600);
scene->set_clear_color(0.08f, 0.08f, 0.12f);
scene->begin_frame();
scene->draw(nativeapp::MeshData::triangle(), nativeapp::Material{});
scene->present();
```

---

## 9. Exemplo completo

Quad 2D colorido, com matriz model column-major vinda de `math.hpp`:

```cpp
#include <nativeapp/graphics_types.hpp>
#include <nativeapp/math.hpp>

using namespace nativeapp;

void desenhar_quad(nativeapp::Scene& scene, float x, float y, float escala) {
    const MeshData mesh = MeshData::quad(100.0f, 100.0f);

    Material material;
    material.color[0] = 1.0f;
    material.color[1] = 0.6f;
    material.color[2] = 0.2f;
    material.color[3] = 1.0f;
    material.depth_test = false;   // sobreposição 2D
    material.blend = true;

    const Mat4 model = mat4_multiply(
        mat4_translate(x, y, 0.0f),
        mat4_scale(escala, escala, 1.0f));

    scene.draw(mesh, material, model.data());
}
```

Sequência típica de quadro:

```cpp
scene.resize(largura, altura);          // projeção ortho2d no backend GLES
scene.set_clear_color(0.0f, 0.0f, 0.0f, 1.0f);
scene.begin_frame();
desenhar_quad(scene, 120.0f, 80.0f, 1.0f);
scene.present();
```

---

## 10. Armadilhas

| Armadilha | Por que acontece | Como evitar |
|---|---|---|
| Inicializar `Vertex` na ordem errada | É agregado: `Vertex{x, y}` preenche x e y; `nz` e a cor já vêm com `1`. | Use a ordem `pos → normal → UV → cor` ou atribua campos pelo nome. |
| Achar que `SpriteVertex` funciona com `MeshData` | `MeshData::vertices` é `std::vector<Vertex>`; não existe conversão no header. | Monte `Vertex` (com `z = 0`) para desenhar via `Scene::draw`. |
| Ultrapassar `max_vertices` (65536) | `add_triangle()`/`add_quad()` checam a capacidade **antes** de qualquer `push_back` e lançam `std::runtime_error` sem mutar a malha. | Trate a exceção ou divida a malha; o limite é 65536 vértices (índice máximo 65535). |
| Achar que `add_quad` gera 4 vértices | Cada triângulo tem 3 vértices próprios: o quad gera **6 vértices** e 6 índices, com `tl`/`br` duplicados. | Conte com 6 vértices por quad ao calcular capacidade; `MeshData::quad()` segue o mesmo padrão. |
| Passar cantos em ordem errada para `add_quad` | A função espera `tl, tr, br, bl` e monta `(tl,tr,br)` + `(tl,br,bl)`. | Respeite a ordem; a diagonal é `tl → br`. |
| Esperar que `quad()` comece em `(0,0)` | É centrado na origem: de `-w/2` a `w/2` e `-h/2` a `h/2`. | Some com `mat4_translate` via parâmetro `model`. |
| Topologia incompatível com os índices | `topology` é livre, e o header não valida a combinação. | Ajuste `topology` junto com a forma como você preenche `indices`. |
| `Material` com textura que não existe | `GLES2Scene::draw_mesh()` tenta `Texture::load()` e cai para `u_use_texture = 0` com aviso no log. | Confirme o caminho relativo em `assets/`. |
| `model` inválido ou apontando para temporário morto | `Scene::draw` recebe `const float*`, não copia os dados. | Guarde o `Mat4` em variável e passe `.data()`; `nullptr` é aceito. |
| Esperar que `model` e `material` sejam ignorados no Vulkan | `VulkanScene::enqueue_draw()` já copia cor, textura, flags e matriz para o `DrawItem`; o UBO é preenchido em `prepare_frame()`. | Preencha `Material`/`model` normalmente; `set_pipeline()` continua necessário para geometria. |
| Chamar `init()` via ponteiro `Scene*` | `init` não faz parte da interface. | Inicialize o objeto concreto antes de guardá-lo como `Scene*`. |
| Usar `begin_frame()` esperando limpeza no Vulkan | O método é vazio; o clear acontece no render pass. | Conte com o fluxo interno do `VulkanScene`. |

---

## 11. Resumo rápido

```cpp
#include <nativeapp/graphics_types.hpp>
#include <nativeapp/math.hpp>

using namespace nativeapp;

// 1) Geometra pronta
MeshData mesh = MeshData::quad(64.0f, 64.0f);   // 6 vértices, 6 índices
// ... ou: MeshData mesh2 = MeshData::triangle();
// ... ou: mesh.topology = Topology::TriangleStrip; (com índices adequados)

// 2) Material
Material mat;                 // branco, sem textura, depth_test/blend = true
mat.color[3] = 0.75f;         // alfa
mat.texture = "textures/icone.png";

// 3) Matriz model (16 floats column-major)
const Mat4 model = mat4_translate(10.0f, 20.0f, 0.0f);

// 4) Desenho
scene.begin_frame();
scene.draw(mesh, mat, model.data());   // nullptr ⇒ identidade no GLES2Scene
scene.present();
```

| Tipo / método | Uso principal |
|---|---|
| `Vertex` | Vértice 3D completo: posição, normal, UV, cor (48 bytes). |
| `SpriteVertex` | Vértice 2D simples: x, y, UV, cor (32 bytes). |
| `Topology` | `Triangles`, `TriangleStrip`, `TriangleFan`, `Lines`, `LineStrip`, `Points`. |
| `MeshData::vertices` / `indices` / `topology` | Dados CPU-side da malha. |
| `MeshData::max_vertices` | `65536` (maior índice `uint16_t`: 65535). |
| `add_triangle` / `add_quad` | Append com checagem de capacidade; 3 / 6 vértices por chamada. |
| `MeshData::quad(w,h)` / `MeshData::triangle()` | Malhas prontas. |
| `Material` | `color[4]`, `texture`, `depth_test`, `blend`. |
| `Scene` | `resize`, `begin_frame`, `present`, `set_clear_color`, `draw`. |
| `GLES2Scene` / `GLES3Scene` / `VulkanScene` | Implementações concretas de `Scene`. |
