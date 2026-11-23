# Guia completo do subsistema gráfico OpenGL ES (`nativeapp/gl.hpp`)

> **Fonte de verdade**: `include/nativeapp/gl.hpp`, complementado por
> `include/nativeapp/graphics_types.hpp`, `include/nativeapp/math.hpp`,
> `include/nativeapp/assets.hpp`, `include/nativeapp/app.hpp` (apenas para o
> ciclo de frames) e pelos exemplos
> `examples/nativeapp-advanced/jni/main.cpp` e
> `examples/nativeapp-gl/jni/main.cpp`.
>
> Todo comportamento descrito existe no código. Quando o texto tira uma
> consequência do código (em vez de transcrever um comentário), isso aparece
> como **Observação**. Quando uma limitação honesta da implementação atual
> precisa ser destacada, isso aparece como **Atenção**.

---

## 1. Visão geral das camadas

O subsistema gráfico é um conjunto de classes C++ em `namespace nativeapp`,
cabeçalho único (`gl.hpp`), que encapsula EGL + OpenGL ES 2/3 para uma
`NativeActivity`. O próprio cabeçalho define as camadas:

```text
  GLContext  — gerencia display/context/surface (sobrevive a cenas)
      │
      ▼
  Shader      — compila vertex/fragment, uniforms
      │
  Mesh        — VBO + IBO reutilizável
      │
  Texture     — carrega PNG/JPEG/WebP/SVG de assets/
      │
      ▼
  GLES2Scene  — orquestra tudo (implementa Scene)
      │
      ▼
  GLES3Scene  — herda tudo e adiciona init(3) + draw_instanced()
```

| Camada | Papel real no código | O que **não** faz |
|---|---|---|
| `GLContext` | `eglGetDisplay`/`eglInitialize`/`eglChooseConfig`/`eglCreateContext`/`eglCreateWindowSurface`, `eglMakeCurrent`, `eglSwapBuffers`, `eglSwapInterval`, destruição EGL | Não desenha, não sabe o que é uma malha; não chama `glViewport` |
| `Shader` | Compila estágios, linka programa, fixa locations 0–3, guarda cache de uniforms, aplica `set_uniform*` | Não faz `use()` implícito nos setters; não gerencia texturas |
| `Mesh` | Faz upload de `MeshData` para VBO/IBO e emite `glDrawElements`/`glDrawArrays` | Não desativa os arrays de atributos após `draw()`; não faz instancing |
| `Texture` | Decode via stb_image embutido, `glTexImage2D`, filtros/wrap, `glActiveTexture`+`glBindTexture` | Não seta o uniform sampler; não gerencia cache de caminhos |
| `GLES2Scene` | Implementa `Scene`: inicializa GL, projection ortho2d, materiais, cache de texturas, `begin_frame`/`present`, `draw`/`draw_mesh`, `render()` legado | Não é proprietária dos ponteiros passados em `set_shader`/`set_mesh` |
| `GLES3Scene` | Pede contexto 3 e oferece `draw_instanced()` | **Não** usa VAOs, `glVertexAttribDivisor` nem `glDrawElementsInstanced` |

Inclua tudo com um dos dois caminhos:

```cpp
#include <nativeapp/nativeapp.hpp>   // umbrella: inclui gl.hpp, app.hpp etc.
// ou
#include <nativeapp/gl.hpp>          // puxa assets.hpp, graphics_types.hpp, math.hpp,
                                     // EGL/egl.h, GLES2/gl2.h, GLES3/gl3.h e stb_image.h
```

### 1.1 Tipos base que a camada GL consome

De `graphics_types.hpp`:

| Tipo | Definição | Uso no GL |
|---|---|---|
| `Vertex` | `x,y,z`; `nx,ny,nz` (padrão `0,0,1`); `u,v`; `r,g,b,a` (padrão `1,1,1,1`) — 12 floats | É o formato que `Mesh::upload()` envia; `Mesh::draw()` mapeia `x→loc 0`, `nx→loc 1`, `u→loc 2`, `r→loc 3` |
| `SpriteVertex` | `x,y,u,v,r,g,b,a` | **Declarado, mas não consumido por nenhum código GL** — `MeshData::vertices` é `std::vector<Vertex>` |
| `Topology` | `Triangles`, `TriangleStrip`, `TriangleFan`, `Lines`, `LineStrip`, `Points` | Traduzido para `GL_*` em `Mesh::gl_mode()` |
| `MeshData` | `vertices`, `indices` (`uint16_t`), `topology` | Geometria CPU-side; vira VBO/IBO no upload |
| `Material` | `color[4]`, `texture` (caminho em `assets/`), `depth_test`, `blend` | Traduzido para uniforms + estados GL em `draw_mesh()` |
| `Scene` | Interface com `resize`, `begin_frame`, `present`, `set_clear_color`, `draw` | Contrato usado por `App::set_scene()` |

De `math.hpp`, as funções que aparecem neste subsistema:

| Função | Papel |
|---|---|
| `Mat4 = std::array<float,16>` | Matriz column-major, compatível com `glUniformMatrix4fv(..., GL_FALSE, ...)` |
| `mat4_identity()` | Matriz identidade; usada como `u_model` padrão e como projection em `render()` |
| `mat4_translate(x,y,z)` | Base de `set_offset()` e das matrizes model dos exemplos |
| `mat4_multiply(a,b)`, `mat4_scale`, `mat4_rotate_x/y/z` | Composição de matrizes model |
| `mat4_ortho2d(w,h)` | Usada por `GLES2Scene::resize()`; equivale a `mat4_ortho(0, w, h, 0, -1, 1)` |

### 1.2 Erros e diagnóstico

```cpp
class GLError : public std::runtime_error {
public:
    explicit GLError(const std::string& message);
};

inline void gl_check(const char* operation);
```

- `GLError` é lançado nas falhas de EGL (`GLContext::init`) e nas falhas de
  compilação/link (`Shader::compile_stage`, `Shader::link`, `Shader::load` sem
  `AAssetManager`).
- `gl_check()` **só chama `glGetError()` quando `APKFAST_DEBUG` está definido**.
  Nesse caso, loga `ANDROID_LOG_WARN` com tag `nativeapp-gl` no formato
  `"%s: glGetError=0x%x"`. Sem a macro, a função vira `(void)operation`. Ela
  apenas avisa — não lança exceção.
- Na prática, `gl_check()` é chamado em `Mesh::draw()` (`"mesh_draw"`) e no fim
  de `GLES2Scene::draw_mesh()` (`"draw_mesh"`).

---

## 2. `GLContext` — Display, contexto e surface EGL

```cpp
class GLContext {
public:
    GLContext() = default;
    ~GLContext();                       // chama destroy()
    GLContext(const GLContext&) = delete;
    GLContext& operator=(const GLContext&) = delete;

    void init(ANativeWindow* window, int client_version = 2);
    void make_current() const;
    void recreate_surface(ANativeWindow* window);
    void set_swap_interval(int interval) const;
    bool swap() const;
    void clear_current() const;
    void destroy();

    int        version() const;
    EGLDisplay display() const;
    EGLContext context() const;
};
```

### 2.1 `init(window, client_version = 2)`

Sequência exata do código:

1. Monta `attribs` com `EGL_WINDOW_BIT`, RGBA 8 bits, `EGL_DEPTH_SIZE 24`,
   `EGL_STENCIL_SIZE 8` e `EGL_RENDERABLE_TYPE` igual a
   `EGL_OPENGL_ES3_BIT` se `client_version >= 3`, senão `EGL_OPENGL_ES2_BIT`.
2. `eglGetDisplay(EGL_DEFAULT_DISPLAY)` → `GLError("eglGetDisplay falhou")` se
   `EGL_NO_DISPLAY`.
3. `eglInitialize(display_, nullptr, nullptr)` → `GLError("eglInitialize falhou")`.
4. **Tentativa GLES3**: se `client_version >= 3`, tenta primeiro um config
   específico (`es3_configs`: `EGL_WINDOW_BIT`, RGB 8, profundidade 24,
   `EGL_OPENGL_ES3_BIT` — sem alpha e sem stencil nesse config). Se
   `eglChooseConfig` falhar ou não retornar configs, faz
   `client_version = 2;  // fallback`.
5. Se ainda não há config, tenta o `attribs` original; se falhar →
   `GLError("eglChooseConfig falhou")`.
6. Cria o contexto com `EGL_CONTEXT_CLIENT_VERSION = client_version` →
   `GLError("eglCreateContext falhou")` se falhar. Grava `version_`.
7. Guarda `window_` e cria a window surface →
   `GLError("eglCreateWindowSurface falhou")` se falhar.
8. Chama `make_current()`.

```cpp
GLContext ctx;
ctx.init(window, 3);        // pede GLES3; pode terminar em 2
LOGI("contexto GLES%d", ctx.version());
if (ctx.version() < 3) {
    // pólem: o fallback já aconteceu dentro de init()
}
```

#### Fallback GLES3 → 2, passo a passo

| Situação | Resultado |
|---|---|
| Dispositivo tem config ES3 | `version_ == 3`, contexto ES3 |
| `eglChooseConfig(es3_configs, ...)` retorna 0 configs | `client_version` passa a `2`; o config genérico `attribs` (com alpha/stencil) é usado; `version_ == 2` |
| Nenhum dos dois configs existe | `GLError` — não há contexto |

> **Atenção:** o fallback é automático e silencioso em `GLContext`. Quem avisa
> o desenvolvedor é `GLES3Scene::init()`, que compara `egl().version() < 3` e
> loga um aviso. `GLES2Scene`, por pedir versão 2, nunca aciona esse caminho.

### 2.2 `make_current()` e `clear_current()`

`make_current()` só chama `eglMakeCurrent` se **display, contexto e surface**
são válidos:

```cpp
eglMakeCurrent(display_, surface_, surface_, context_);  // draw = read = surface
```

`clear_current()` chama `eglMakeCurrent(display_, EGL_NO_SURFACE,
EGL_NO_SURFACE, EGL_NO_CONTEXT)` (se houver display). `recreate_surface()` usa
isso internamente antes de destruir a surface antiga.

> **Observação:** quase toda operação das cenas começa com
> `egl_.make_current()` (`resize`, `draw`, `begin_frame`, `present` e
> `destroy`). Isso reconecta o contexto após uma recriação de surface ou após
> outra thread ter feito current.

### 2.3 `recreate_surface(window)`

1. Sai imediatamente se `display_` ou `config_` são inválidos.
2. `make_current()`.
3. Se existe surface: desfaz current, `eglDestroySurface`, zera `surface_`.
4. Atualiza `window_`; se o ponteiro não for nulo, cria nova surface com o
   **mesmo `config_`**.
5. `make_current()` novamente.

> **Observação:** `recreate_surface()` **não** chama `resize()` e não altera
> `projection_`. Depois de recriar a surface, chame `GLES2Scene::resize()` com
> as novas dimensões se elas mudaram.

### 2.4 `set_swap_interval`, `swap`, `destroy` e getters

| Método | Comportamento |
|---|---|
| `set_swap_interval(int) const` | `eglSwapInterval(display_, interval)` se houver display (`0` desliga vsync; `1` sincroniza) |
| `swap() const` | `eglSwapBuffers(display_, surface_) == EGL_TRUE`; retorna `false` se não há display ou surface |
| `destroy()` | Destrói surface, contexto, chama `eglTerminate` e zera display/config/window; é idempotente (`display_ == EGL_NO_DISPLAY` → retorna) |
| `version()`, `display()`, `context()` | Acesso ao estado atual; `version()` reflete o fallback |

### 2.5 Por que **não** chamar `resize()` dentro de `init()`

`GLContext::init()` não configura viewport — quem faz isso é
`GLES2Scene::resize()`, que chama `glViewport(0,0,w,h)` **e** recalcula
`projection_ = mat4_ortho2d(w,h)`. A cena até tenta um resize automático dentro
de `init(window, version)`:

```cpp
const int w = window ? ANativeWindow_getWidth(window) : 0;
const int h = window ? ANativeWindow_getHeight(window) : 0;
if (w > 0 && h > 0) resize(w, h);
```

Mas isso é apenas uma conveniência, não uma garantia:

- `ANativeWindow_getWidth/getHeight` podem retornar `0` ou valores ainda não
  estabilizados no momento da criação da surface — nesse caso o `if` não roda e
  o viewport/projection ficam com os valores padrão (`projection_ = identity`).
- A fonte autoritativa de dimensões é o callback `on_surface_changed(w,h)` do
  `App`, que também atualiza `width()`/`height()` do app.
- `resize()` **sobrescreve** qualquer `set_projection()` anterior; chamá-lo
  "por dentro" do init atrapalharia quem define uma projeção customizada antes
  de inicializar.
- Após `recreate_surface()` ou `destroy()`+`init()`, a surface pode ter mudado
  de tamanho.

É por isso que os exemplos tratam as duas coisas separadamente:

```cpp
// examples/nativeapp-gl/jni/main.cpp
void on_surface_created() override {
    scene_.init(window());
    scene_.resize(width(), height());   // dimensões definitivas
}
void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
```

```cpp
// examples/nativeapp-advanced/jni/main.cpp
void on_surface_created() override { scene_.init(window()); }
void on_surface_changed(int w, int h) override { scene_.resize(w, h); }
```

> **Regra prática:** `init()` cria o contexto; `resize()` define viewport e
> projeção. Chame `resize()` explicitamente no `on_surface_created()` e no
> `on_surface_changed()`, sobretudo depois de recriar a surface.

---

## 3. `Shader` — Compilação, uniforms e locations fixas

```cpp
class Shader {
public:
    Shader() = default;
    ~Shader();                          // chama destroy()
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    void compile(const std::string& vertex_source, const std::string& fragment_source);
    void load(const std::string& vertex_path, const std::string& fragment_path);

    void use() const;

    void set_uniform(const std::string& name, float value);
    void set_uniform(const std::string& name, int value);
    void set_uniform(const std::string& name, float x, float y);
    void set_uniform(const std::string& name, float x, float y, float z);
    void set_uniform(const std::string& name, float x, float y, float z, float w);
    void set_uniform_mat4(const std::string& name, const float* matrix);

    GLint  attrib(const std::string& name);
    GLuint program() const;
    void   destroy();
};
```

### 3.1 `compile()` com fontes inline

```cpp
Shader shader;
shader.compile(R"(
    attribute vec3 a_position;
    attribute vec3 a_normal;
    attribute vec2 a_uv;
    attribute vec4 a_color;
    uniform mat4 u_projection;
    uniform mat4 u_model;
    varying vec4 v_color;
    void main() {
        v_color = a_color;
        gl_Position = u_projection * u_model * vec4(a_position, 1.0);
    }
)", R"(
    precision mediump float;
    varying vec4 v_color;
    void main() { gl_FragColor = v_color; }
)");
shader.use();
```

O que acontece internamente:

1. `compile_stage(GL_VERTEX_SHADER, source)` cria o shader, chama
   `glShaderSource`, `glCompileShader` e verifica `GL_COMPILE_STATUS`.
2. Se o vertex compila mas o fragment falha, o vertex é deletado antes de
   relançar a exceção.
3. `link(vs, fs)` cria o programa, anexa os dois estágios, chama
   `glBindAttribLocation` (ver §3.4), linka e verifica `GL_LINK_STATUS`.
4. Em caso de falha de link, deleta shaders e programa, zera `program_` e
   lança `GLError("Link falhou: " + log)`. O log vem de
   `glGetProgramInfoLog` em buffer de 2048 bytes.
5. Sucesso: os dois shaders são deletados (o programa linkado mantém o código).

### 3.2 `load()` a partir de `assets/`

```cpp
shader.load("shaders/cena.vert", "shaders/cena.frag");
```

```cpp
auto& assets = Assets::instance(nullptr);
if (!assets.manager()) throw GLError("AAssetManager indisponível");
compile(assets.read_text(vertex_path), assets.read_text(fragment_path));
```

> **Observação:** `Assets::instance(nullptr)` funciona porque o singleton já
> foi criado pelo `App`, que injeta o `AAssetManager` em
> `Assets::instance(app_->activity).set_activity(app_->activity)` antes de
> qualquer cena carregar assets. Se `manager()` for nulo, `load()` lança
> `GLError`. Arquivos inexistentes viram string vazia via `read_text()` e a
> falha aparece como `GLError("Shader falhou: ...")` na compilação.
> Veja §5 para o comportamento equivalente de `Texture::load()`.

### 3.3 `set_uniform*` — todos os overloads reais

| Assinatura | Chamada GL |
|---|---|
| `set_uniform(name, float value)` | `glUniform1f` |
| `set_uniform(name, int value)` | `glUniform1i` |
| `set_uniform(name, float x, float y)` | `glUniform2f` |
| `set_uniform(name, float x, float y, float z)` | `glUniform3f` |
| `set_uniform(name, float x, float y, float z, float w)` | `glUniform4f` |
| `set_uniform_mat4(name, const float* matrix)` | `glUniformMatrix4fv(loc, 1, GL_FALSE, matrix)` |

```cpp
shader.use();                                  // glUniform atua no programa atual
shader.set_uniform("u_tempo", 1.5f);
shader.set_uniform("u_modo", 1);
shader.set_uniform("u_resolucao", 1280.0f, 720.0f);
shader.set_uniform("u_luz", 0.2f, 0.4f, 0.8f);
shader.set_uniform("u_cor", 1.0f, 0.5f, 0.2f, 1.0f);
shader.set_uniform_mat4("u_model", model.data());
```

> **Observação:** `set_uniform*` **não** chamam `use()`. Chame `shader.use()`
> antes. `set_uniform_mat4` recebe ponteiro cru — `Mat4` (`std::array<float,16>`)
> deve ser passado com `.data()`; a matriz é column-major e `GL_FALSE` indica
> que não deve ser transposta.

### 3.4 Locations cacheadas e bind de atributos 0–3

- `loc(name)` guarda o resultado de `glGetUniformLocation` num
  `std::unordered_map<std::string, GLint>`. A primeira consulta chama GL; as
  seguintes reusam o valor. `destroy()` limpa o mapa.
- `attrib(name)` **não é cacheado**: chama `glGetAttribLocation` diretamente.
- No `link()`, antes de linkar, o código fixa:

| Location | Nome do atributo | Tamanho em `Mesh::draw()` |
|---|---|---|
| 0 | `a_position` | 3 floats (`Vertex::x`) |
| 1 | `a_normal` | 3 floats (`Vertex::nx`) |
| 2 | `a_uv` | 2 floats (`Vertex::u`) |
| 3 | `a_color` | 4 floats (`Vertex::r`) |

> **Atenção:** se um uniform não existe (ou foi otimizado pelo compilador),
> `glGetUniformLocation` retorna `-1` e esse `-1` fica cacheado; chamar
> `glUniform*` com location `-1` é um no-op — não gera erro visível. Isso
> explica por que um nome de uniform errado "não faz nada" silenciosamente.
> Shaders customizados usados com `Mesh::draw()` devem declarar os atributos
> acima (ou pelo menos consumir as locations 0–3 com o mesmo layout).

---

## 4. `Mesh` e `MeshData` — Geometria CPU-side e GPU-side

### 4.1 Construção de `MeshData`

```cpp
struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    Topology topology = Topology::Triangles;

    void add_triangle(const Vertex& a, const Vertex& b, const Vertex& c);
    void add_quad(const Vertex& tl, const Vertex& tr,
                  const Vertex& br, const Vertex& bl);

    static MeshData quad(float w = 1.0f, float h = 1.0f);
    static MeshData triangle();
};
```

| Método | Efeito real |
|---|---|
| `add_triangle(a,b,c)` | Faz `base = static_cast<uint16_t>(vertices.size())`, empurra os 3 vértices e os índices `base, base+1, base+2` |
| `add_quad(tl,tr,br,bl)` | `add_triangle(tl,tr,br)` + `add_triangle(tl,br,bl)` → 4 vértices, 6 índices |
| `quad(w=1,h=1)` | Quadrado centrado na origem (`±w/2`, `±h/2`), normais `(0,0,1)`, UV `(0,0)→(1,1)`, cor branca |
| `triangle()` | 3 vértices em `(0,0.5)`, `(-0.5,-0.5)`, `(0.5,-0.5)` (faixa NDC), UV e cores vermelho/verde/azul |

```cpp
MeshData losango;
Vertex v;                 // cor e normais já vêm com defaults
v.x = 0.0f; v.y = 0.5f; v.r = 1.0f; v.g = 0.9f;
Vertex dir = v;
dir.x = 0.4f; dir.y = 0.0f;
dir.r = 0.2f; dir.g = 0.6f; dir.b = 1.0f;
Vertex esq = dir; esq.x = -0.4f;
Vertex fim = v; fim.y = -0.5f;
losango.add_triangle(v, dir, esq);
losango.add_triangle(dir, fim, esq);
losango.topology = Topology::Triangles;   // já é o padrão
```

### 4.2 `Mesh` — upload e draw

```cpp
class Mesh {
public:
    Mesh() = default;
    ~Mesh();                            // chama destroy()
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void upload(const MeshData& data);
    void draw() const;
    void destroy();

    GLsizei vertex_count() const;
    void set_vertex_count(GLsizei count);
};
```

**`upload(data)`**:

- Cria VBO se ainda não existe; cria IBO se ainda não existe **e**
  `data.indices` não está vazio.
- `glBufferData(GL_ARRAY_BUFFER, vertices.size()*sizeof(Vertex), ..., GL_STATIC_DRAW)`.
- Guarda `vertex_count_` e `index_count_`.
- Se há IBO e `index_count_ > 0`, faz `glBufferData(GL_ELEMENT_ARRAY_BUFFER,
  indices.size()*sizeof(uint16_t), ..., GL_STATIC_DRAW)`.
- Copia `data.topology` para `topology_`.

> **Observação:** fazer upload de uma `MeshData` sem índices num `Mesh` que já
> tinha IBO mantém o IBO vivo, mas `index_count_` zera — o `draw()` passa a
> usar `glDrawArrays`. O contrário também vale: trocar a topologia exige novo
> upload.

**`draw() const`**:

1. Sai se não há VBO.
2. Seta `GL_ARRAY_BUFFER` e `stride = sizeof(Vertex)`, com quatro
   `glVertexAttribPointer`: 0←`x` (3f), 1←`nx` (3f), 2←`u` (2f), 3←`r` (4f).
3. `glEnableVertexAttribArray(0..3)`.
4. Escolhe o modo pela topologia e desenha:
   - com IBO e índices: `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_)` +
     `glDrawElements(mode, index_count_, GL_UNSIGNED_SHORT, nullptr)`;
   - sem índices: `glDrawArrays(mode, 0, vertex_count_)`.
5. Chama `gl_check("mesh_draw")`.

| `Topology` | `GLenum` |
|---|---|
| `Triangles` (inclusive o default do `switch`) | `GL_TRIANGLES` |
| `TriangleStrip` | `GL_TRIANGLE_STRIP` |
| `TriangleFan` | `GL_TRIANGLE_FAN` |
| `Lines` | `GL_LINES` |
| `LineStrip` | `GL_LINE_STRIP` |
| `Points` | `GL_POINTS` |

> **Atenção:** `Mesh::draw()` **liga** os arrays 0–3 e **não os desliga**. Em
> `GLES2Scene::draw_mesh()` isso é compensado logo após a chamada. Se você
> chamar `mesh.draw()` diretamente, desative você mesmo os quatro arrays —
> veja §10.

### 4.3 Limite de 65535 índices

`MeshData::indices` é `std::vector<uint16_t>` e `draw()` usa
`GL_UNSIGNED_SHORT`. Portanto:

- índice máximo representável: **65535** (65536 vértices endereçáveis no total);
- `add_triangle()` converte `vertices.size()` para `uint16_t` **sem checagem** —
  passar de 65535 vértices faz o índice "dar a volta" silenciosamente;
- o limite vale para `Mesh::upload()` e para o `temporary_mesh_` usado por
  `GLES2Scene::draw()`.

Para geometria maior, divida em vários `MeshData`/`Mesh` — o cabeçalho não
oferece índices de 32 bits.

### 4.4 Sobre instancing

`Mesh` **não** tem `draw_instanced()`. O único método com esse nome está em
`GLES3Scene` (§7.2) e, na implementação atual, ele chama `draw()` em loop — ou
seja, também passa pelo upload do `temporary_mesh_` da cena a cada iteração.

---

## 5. `Texture` — stb_image, WebP/SVG e cache na cena

```cpp
class Texture {
public:
    Texture() = default;
    ~Texture();                                // chama destroy()
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    bool load(const std::string& path);                 // detecta o formato
    bool load_webp_asset(const std::string& path);
    bool load_svg_asset(const std::string& path,
                        int width = 0, int height = 0);
    bool load_rgba(const uint8_t* rgba, int width, int height);
    void bind(unsigned int unit = 0) const;
    void destroy();

    int    width() const;
    int    height() const;
    GLuint id() const;
};
```

### 5.1 `load(path)`

Desde a v2.2.0 o formato é escolhido por magic bytes: PNG/JPEG continuam no
stb_image, WebP usa `BitmapFactory` + `AndroidBitmap_*` via JNI, e SVG é
rasterizado pelo NanoSVG. Veja [image-formats.md](image-formats.md).

```cpp
auto& assets = Assets::instance(nullptr);
if (!assets.manager()) return false;
const auto data = assets.read(path);
if (data.empty()) return false;

int width = 0, height = 0, channels = 0;
stbi_uc* pixels = stbi_load_from_memory(
    data.data(), static_cast<int>(data.size()), &width, &height, &channels, 4);
if (!pixels) return false;

if (!texture_) glGenTextures(1, &texture_);
glBindTexture(GL_TEXTURE_2D, texture_);
glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
             GL_RGBA, GL_UNSIGNED_BYTE, pixels);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
stbi_image_free(pixels);
```

Pontos importantes:

- **stb_image está embutido**: `gl.hpp` define `STB_IMAGE_IMPLEMENTATION`,
  inclui `stb_image.h` e removes a macro. Não é preciso compilar um `.c` extra.
- O `req_comp = 4` força RGBA na saída, então PNG com alpha e JPEG funcionam
  pelo mesmo caminho.
- Filtros são `GL_LINEAR` em minificação/magnificação; wrap é
  `GL_CLAMP_TO_EDGE` em S e T. Não há mipmaps.
- Retorna `false` (sem exceção) se: não há `AAssetManager`, o arquivo não
  existe/está vazio, ou o decode falha.
- `width()`, `height()` e `id()` devolvem o estado atual; antes de um load
  bem-sucedido, `id() == 0` e as dimensões são `0`.

### 5.2 `bind(unit = 0)`

```cpp
texture.bind(0);        // glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, id)
texture.bind(1);        // GL_TEXTURE1
```

> **Observação:** `bind()` não seta o uniform sampler. Quem usa
> `GLES2Scene::draw_mesh()` recebe `u_texture = 0` automaticamente; com um
> shader próprio, sete `shader.set_uniform("u_texture", unidade)` depois do
> `bind(unidade)`.

### 5.3 Texturas e o `Assets` do `App`

Diferente de `Shader::load()` (que lança `GLError` sem manager),
`Texture::load()` apenas devolve `false`. O `App` já injeta o
`AAssetManager` antes dos callbacks de surface:

```cpp
// app.hpp, ao processar o comando de criação
Assets::instance(app_->activity).set_activity(app_->activity);
```

Se você usa `GLContext`/`Texture` **fora** de um `App`, precisa garantir isso
você mesmo (passando a `activity` na primeira chamada de `Assets::instance` ou
chamando `set_activity`). O caminho passado a `load()` é relativo ao diretório
`assets/` do APK.

`GLES2Scene` mantém um cache `std::unordered_map<std::string,
std::unique_ptr<Texture>> textures_`, chaveado pelo **caminho** usado em
`Material::texture`. Veja o detalhe perigoso desse cache em §6.4 e §10.

---

## 6. `GLES2Scene` — Orquestração e os dois caminhos de desenho

```cpp
class GLES2Scene : public Scene {
public:
    GLES2Scene() = default;
    ~GLES2Scene() override;              // chama destroy()

    virtual void init(ANativeWindow* window);           // init(window, 2)

    void set_shader(Shader* shader);
    void set_mesh(Mesh* mesh);

    void resize(int width, int height) override;
    void set_clear_color(float r, float g, float b, float a = 1.0f) override;
    void set_offset(float x, float y);
    void set_projection(const Mat4& projection);
    void set_swap_interval(int interval);

    void draw(const MeshData& data, const Material& material,
              const float* model = nullptr) override;
    void draw_mesh(const Mesh& mesh, const Material& material,
                   const float* model = nullptr);

    void begin_frame() override;
    void present() override;
    void render();                       // compatibilidade (NDC)

    void destroy();

    GLContext&       egl();
    const GLContext& egl() const;

protected:
    void init(ANativeWindow* window, int client_version);
};
```

### 6.1 `init()`

`init(window)` chama a sobrecarga protegida `init(window, 2)`. Essa sobrecarga:

1. Retorna se `initialized_` já é `true` (idempotente).
2. `egl_.init(window, client_version)`.
3. `glEnable(GL_DEPTH_TEST)`, `glEnable(GL_BLEND)`,
   `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`.
4. `glClearColor(clear_...)` — padrão `{0.08f, 0.08f, 0.12f, 1.0f}`.
5. `shader_.compile(kVertexShader, kFragmentShader)` — shaders embutidos.
6. `mesh_.upload(default_mesh())` — `MeshData::triangle()`.
7. Tenta `resize()` com `ANativeWindow_getWidth/Height` se ambos > 0 (§2.5).
8. Marca `initialized_ = true`.

> **Observação:** se `init()` lançar `GLError` antes de `initialized_ = true`,
> `destroy()` não limpa nada (ele começa com `if (!initialized_) return;`).
> tratamento de erro é responsabilidade do chamador.

### 6.2 Shader embutido (contrato dos uniforms)

Vertex:

```glsl
attribute vec3 a_position;
attribute vec3 a_normal;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform mat4 u_projection;
uniform mat4 u_model;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = u_projection * u_model * vec4(a_position, 1.0);
}
```

Fragment:

```glsl
precision mediump float;
varying vec2 v_uv;
varying vec4 v_color;
uniform vec4 u_color;
uniform sampler2D u_texture;
uniform int u_use_texture;
void main() {
    vec4 base = u_use_texture == 1 ? texture2D(u_texture, v_uv) : vec4(1.0);
    gl_FragColor = base * v_color * u_color;
}
```

> **Observação:** `a_normal` é declarado e recebe location 1, mas não é usado
> no cálculo do vertex shader. A cor final é
> `texture * v_color * u_color` — a cor por vértice multiplica a cor do
> `Material`.

### 6.3 `set_shader`, `set_mesh`, `resize`, projeção e estados

| Método | Efeito |
|---|---|
| `set_shader(Shader*)` | Guarda ponteiro **não proprietário**; `nullptr` volta ao shader embutido. O shader deve ser compatível com os uniforms/atributos de `draw_mesh()` |
| `set_mesh(Mesh*)` | Guarda ponteiro **não proprietário**; é a mesh usada por `render()` (não afeta `draw()`) |
| `resize(w,h)` | `make_current()`, `glViewport(0,0,w,h)`, `projection_ = mat4_ortho2d(w,h)` |
| `set_clear_color(r,g,b,a=1)` | Atualiza `clear_` — o `glClearColor` é aplicado em `begin_frame()` (e no `init()`) |
| `set_offset(x,y)` | Compatibilidade: `model_ = mat4_translate(x,y,0)`; usado por `render()` |
| `set_projection(const Mat4&)` | Substitui `projection_` — **será sobrescrito pelo próximo `resize()`** |
| `set_swap_interval(int)` | Repassa a `egl_.set_swap_interval()` |
| `egl()` | Acesso ao `GLContext` interno (útil para `egl().version()` em `on_ready()`) |

`mat4_ortho2d(w,h)` = `mat4_ortho(0, w, h, 0, -1, 1)`, ou seja:

| Eixo | Intervalo | Origem |
|---|---|---|
| X | `0 … width` | esquerda |
| Y | `height … 0` | **topo** (Y cresce para baixo) |
| Z | `-1 … 1` | — |

É por isso que o exemplo avançado usa `width()/2`, `height()/2` e `150.0f`
diretamente como coordenadas de tela.

### 6.4 `draw()` e `draw_mesh()`

```cpp
void draw(const MeshData& data, const Material& material,
          const float* model = nullptr);

void draw_mesh(const Mesh& mesh, const Material& material,
               const float* model = nullptr);
```

`draw()`:

1. Retorna se a cena não foi inicializada.
2. `egl_.make_current()`.
3. `temporary_mesh_.upload(data)`.
4. Chama `draw_mesh(temporary_mesh_, material, model)`.

`draw_mesh()` (o coração do desenho):

1. Escolhe `shader_override_` se definido, senão `shader_`, e chama `use()`.
2. Uniforms: `u_projection = projection_`, `u_model = model ? *model : identidade`,
   `u_color = material.color[0..3]`, `u_use_texture = material.texture.empty() ? 0 : 1`.
3. Se `material.texture` não está vazio: procura no cache por caminho; se não
   existe, cria `Texture`, chama `load()`. Sucesso → guarda no mapa; falha →
   loga `ANDROID_LOG_WARN "nativeapp-gl", "Textura nao carregada: %s"` e seta
   `u_use_texture = 0`. Depois disso, chama `it->second->bind(0)` e seta
   `u_texture = 0`.
4. Aplica `material.depth_test`/`material.blend` com `glEnable`/`glDisable`.
5. Chama `mesh.draw()` e, em seguida, `glDisableVertexAttribArray(0..3)`.
6. Chama `gl_check("draw_mesh")`.

> **Atenção (bug real a evitar):** quando `texture->load()` falha, o código
> **não** insere a textura no mapa, mas ainda executa `it->second->bind(0)` com
> `it == textures_.end()`. Isso é dereferência de iterator inválido
> (comportamento indefinido). Na prática: **use apenas caminhos de textura que
> você confirmou que carregam** (por exemplo, validando com
> `Texture::load()` antes de preencher `Material::texture`). O warning de
> "nao carregada" é emitido, mas a chamada seguinte já é problemática.

> **Observação:** `temporary_mesh_` é **um único** objeto reaproveitado. Cada
> `draw()` faz novo `glBufferData` — ótimo para geometria dinâmica, ruim para
> dezenas de objetos estáticos. Para malhas reutilizáveis, faça
> `mesh.upload(data)` uma vez e chame `draw_mesh(mesh, material, model)`.

### 6.5 `begin_frame()` / `present()` — boundary de frame

```cpp
void begin_frame() override {
    if (!initialized_) return;
    egl_.make_current();
    // Restaura defaults para que um material anterior não vaze estado.
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(clear_[0], clear_[1], clear_[2], clear_[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void present() override {
    if (!initialized_) return;
    egl_.make_current();
    if (!egl_.swap()) {
        __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                            "eglSwapBuffers falhou");
    }
}
```

`begin_frame()` restaura depth/blend/blendFunc/clearColor e faz o clear. Isso
é o que impede que um `Material` com `depth_test=false` ou `blend=false` do
frame anterior vaze para o próximo. `present()` troca os buffers.

### 6.6 `render()` vs `draw()` — a diferença que mais confunde

```cpp
void render() {
    // Compatibilidade: clear + desenha a mesh padrão em NDC + present.
    const Mat4 saved = projection_;
    projection_ = mat4_identity();
    begin_frame();
    Material material;
    draw_mesh(mesh_override_ ? *mesh_override_ : mesh_, material, model_.data());
    projection_ = saved;
    present();
}
```

| | `draw(data, material, model)` | `render()` |
|---|---|---|
| Origem da geometria | `MeshData` passado por argumento | `mesh_` (triângulo embutido) ou o ponteiro de `set_mesh()` |
| Projection usada | `projection_` **do usuário** (ortho2d de `resize()`, ou `set_projection()`) | **Identidade**, ignorando `resize()` e `set_projection()` |
| Material | `Material` passado | `Material material;` — branco, sem textura, depth/blend ligados |
| Matriz model | parâmetro `model` (`nullptr` → identidade) | `model_`, controlada por `set_offset()` |
| Faz clear? | **Não** | **Sim**, via `begin_frame()` |
| Faz swap? | **Não** | **Sim**, via `present()` |
| Uso típico | Chamado dentro de `on_draw()` com `set_scene(&cena)` | Chamado dentro de `on_draw()` **sem** `set_scene()` |

Por que a identidade em `render()`? A mesh legada (`MeshData::triangle()`) tem
coordenadas `-0.5 … 0.5` — já são NDC. A ortho2d de `resize()` espera pixels
`0 … width` / `0 … height`; aplicá-la ao triângulo legado o jogaria para fora
da tela. O código salva a `projection_` do usuário, troca por identidade só
durante o desenho e restaura depois — logo, um `draw()` feito depois de
`render()` continua usando a projeção correta.

Os dois exemplos mostram os dois estilos válidos:

```cpp
// examples/nativeapp-advanced/jni/main.cpp — estilo "App gerencia o frame"
void on_ready() override { set_scene(&scene_); }
void on_draw() override {
    scene_.draw(triangle_, material, mat4_multiply(tri_model, tri_scale).data());
    scene_.draw(quad_, material, model_.data());
}   // sem begin_frame()/present(): o App já faz isso
```

```cpp
// examples/nativeapp-gl/jni/main.cpp — estilo "cena legada gerencia o frame"
// (sem set_scene)
void on_draw() override {
    scene_.set_offset(ox * 0.3f, oy * 0.3f);
    scene_.render();   // clear + draw em NDC + swap
}
```

Nesse segundo exemplo, `ox`/`oy` vêm de
`ox = (touch_x/width)*2 - 1`, `oy = 1 - (touch_y/height)*2` — conversão de
pixels de toque para NDC, coerente com a projection identidade de `render()`.

### 6.7 `destroy()`

Só age se `initialized_`. Faz `make_current()`, limpa o cache de texturas,
destrói `temporary_mesh_`, `mesh_` e `shader_`, chama `egl_.destroy()` e zera
`initialized_`.

> **Observação:** ponteiros registrados com `set_shader()`/`set_mesh()` não são
> deletados — a vida útil deles é de quem os criou.

---

## 7. `GLES3Scene` — Contexto 3 e `draw_instanced()`

```cpp
class GLES3Scene : public GLES2Scene {
public:
    using GLES2Scene::init;                       // expõe init(window, version)

    void init(ANativeWindow* window) override;    // pede versão 3

    void draw_instanced(const MeshData& data, const Material& material,
                        const std::vector<Mat4>& models);
};
```

### 7.1 `init(window)` pede versão 3 de verdade

```cpp
void init(ANativeWindow* window) override {
    GLES2Scene::init(window, 3);
    if (egl().version() < 3) {
        __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                            "GLES3 solicitado mas contexto GLES%d em uso",
                            egl().version());
    }
}
```

Ou seja: a diferença em relação a `GLES2Scene` é o argumento `3` passado ao
`GLContext`, que tenta o config ES3 e aplica o fallback descrito em §2.1. O
warning usa a tag `nativeapp-gl`. `using GLES2Scene::init;` reexpõe a
sobrecarga `init(window, client_version)`; chamar `init(window, 2)` numa
`GLES3Scene` é possível e pede GLES2 explicitamente.

> **Atenção:** apesar do comentário do cabeçalho ("GLES3Scene — VAOs,
> instancing"), o código **não cria VAOs**, não chama
> `glGenVertexArrays`/`glBindVertexArray`, `glVertexAttribDivisor` nem
> `glDrawElementsInstanced`. Toda a funcionalidade de desenho vem de
> `GLES2Scene`.

### 7.2 `draw_instanced()` — implementação honesta

```cpp
void draw_instanced(const MeshData& data, const Material& material,
                    const std::vector<Mat4>& models) {
    if (models.empty()) return;
    egl().make_current();
    // GLES3 permite instancing via divisor de atributo; implementação
    // simplificada usando uniforms por chamada (compatível com GLES2).
    for (const auto& model : models) {
        draw(data, material, model.data());
    }
}
```

O que isso significa na prática:

| Aspecto | Comportamento atual |
|---|---|
| Chamadas de draw | **N** chamadas (`N = models.size()`), uma por matriz |
| Upload de geometria | **N** uploads: cada `draw()` reaproveita `temporary_mesh_` e refaz `glBufferData` |
| Uniforms | Cada iteração seta `u_model` com a matriz daquela instância |
| Compatibilidade | Funciona até em contexto GLES2, porque usa apenas `draw()` |
| VAOs / divisor / `glDrawElementsInstanced` | **Não existem** nesta versão do cabeçalho |
| `models` vazio | Retorno imediato, sem desenho |

Se você esperava ganho de performance, ele **não está implementado**: hoje é um
laço de uniforms com a mesma custo por instância de um `draw()` normal.

---

## 8. Exemplo completo — cena com `MeshData` custom, cor, matriz model e assets

Exemplo autocontido no estilo do `nativeapp-advanced`: cena gerenciada pelo
`App`, geometria construída à mão, `Material` com cor, `Texture`/shader vindos
de `assets/` e matriz model animada.

```cpp
#include <nativeapp/nativeapp.hpp>

#include <android/log.h>

#define LOG_TAG "oficina-gl"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...)  __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)

using namespace nativeapp;

class OficinaApp : public App {
public:
    explicit OficinaApp(android_app* app) : App(app) {}

private:
    void on_ready() override {
        // O App já injetou o AAssetManager (Assets::instance(activity)...).
        // Isto é explícito apenas para mostrar o ponto de entrada dos assets:
        Assets::instance(activity()).set_activity(activity());

        LOGI("on_ready - GLES%d", scene_.egl().version());
        set_scene(&scene_);  // App passa a chamar begin_frame()/present()
        scene_.set_clear_color(0.05f, 0.05f, 0.10f, 1.0f);

        // --- Geometria custom -------------------------------------------------
        // Losango em NDC, montado com add_triangle(); cada vértice tem cor.
        Vertex topo;
        topo.x = 0.0f; topo.y = 0.5f;
        topo.r = 1.0f; topo.g = 0.9f; topo.b = 0.2f;

        Vertex direita = topo;
        direita.x = 0.4f; direita.y = 0.0f;
        direita.r = 1.0f; direita.g = 0.4f; direita.b = 0.2f;

        Vertex esquerda = direita;
        esquerda.x = -0.4f;
        esquerda.r = 0.2f; esquerda.g = 0.8f; esquerda.b = 1.0f;

        Vertex baixo = topo;
        baixo.y = -0.5f;
        baixo.r = 0.4f; baixo.g = 0.2f; baixo.b = 1.0f;

        custom_.add_triangle(topo, direita, esquerda);
        custom_.add_triangle(direita, baixo, esquerda);

        // --- Material com cor -------------------------------------------------
        material_.color[0] = 1.0f;
        material_.color[1] = 1.0f;
        material_.color[2] = 1.0f;
        material_.color[3] = 1.0f;

        // --- Assets: textura --------------------------------------------------
        // Valide antes de colocar o caminho no Material: draw_mesh() tenta
        // carregar Material::texture por conta própria (veja §6.4/§10).
        Texture prova;
        if (prova.load("textures/tile.png")) {
            LOGI("textura %dx%d", prova.width(), prova.height());
            material_.texture = "textures/tile.png";
        } else {
            LOGW("textures/tile.png indisponivel - seguindo sem textura");
            material_.texture.clear();
        }

        // --- Assets: shader opcional -----------------------------------------
        // O shader custom deve respeitar u_projection/u_model/u_color/
        // u_use_texture/u_texture e os atributos 0..3 usados por Mesh::draw().
        try {
            custom_shader_.load("shaders/custom.vert", "shaders/custom.frag");
            scene_.set_shader(&custom_shader_);
        } catch (const GLError& e) {
            LOGW("shader de assets falhou (%s) - usando o embutido", e.what());
            scene_.set_shader(nullptr);
        }
    }

    void on_surface_created() override {
        scene_.init(window());
        scene_.resize(width(), height());
    }

    void on_surface_destroyed() override { scene_.destroy(); }

    void on_surface_changed(int w, int h) override { scene_.resize(w, h); }

    void on_update(float dt) override {
        tempo_ += dt;
        // Sprite em coordenadas de tela: centro + escala em pixels.
        const Mat4 trans = mat4_translate(
            width() * 0.5f + std::sin(tempo_) * width() * 0.25f,
            height() * 0.5f + std::cos(tempo_) * height() * 0.20f,
            0.0f);
        const Mat4 giro  = mat4_rotate_z(tempo_ * 1.2f);
        const Mat4 escala = mat4_scale(120.0f, 120.0f, 1.0f);
        model_ = mat4_multiply(trans, mat4_multiply(giro, escala));
    }

    void on_draw() override {
        // projection_ é a ortho2d de resize(): trabalhamos em pixels.
        scene_.draw(custom_, material_, model_.data());
    }

    void on_key(int32_t code, bool down) override {
        if (down && code == AKEYCODE_BACK) request_exit();
    }

    GLES2Scene scene_;
    Shader     custom_shader_;
    MeshData   custom_;
    Material   material_;
    Mat4       model_ = mat4_identity();
    float      tempo_ = 0.0f;
};

APP_MAIN(OficinaApp)
```

Variantes úteis, já suportadas pelo cabeçalho:

```cpp
// (a) Malha estática reutilizável: upload uma vez, draw_mesh many vezes.
Mesh gpu;
gpu.upload(custom_);
scene_.draw_mesh(gpu, material_, model_.data());   // sem reupload

// (b) Instâncias via GLES3Scene (hoje: loop de draw()).
GLES3Scene c3;
c3.init(window());              // tenta 3, faz fallback para 2
c3.draw_instanced(custom_, material_, {mat4_translate(10, 0, 0),
                                       mat4_translate(20, 0, 0)});
```

> **Observação sobre o exemplo:** ele usa `set_scene(&scene_)`, então
> `on_draw()` faz **somente** `draw()` — `begin_frame()`/`present()` são do
> `App`. Se você preferir o estilo legado, remova o `set_scene()` e troque o
> corpo de `on_draw()` por `scene_.render()` (que usa NDC e o `model_` de
> `set_offset()`); não faça os dois ao mesmo tempo (§10).

---

## 9. Tabela completa de classes e métodos públicos

### 9.1 Free functions e exceções

| Nome | Assinatura | Descrição |
|---|---|---|
| `GLError` | `explicit GLError(const std::string&)` | Exceção de GL/EGL (`std::runtime_error`) |
| `gl_check` | `inline void gl_check(const char* operation)` | `glGetError()` + log **somente** com `APKFAST_DEBUG` |

### 9.2 `GLContext`

| Método | Modificador | Descrição |
|---|---|---|
| `GLContext()` | — | Construtor padrão |
| `~GLContext()` | — | Chama `destroy()` |
| `init(ANativeWindow*, int = 2)` | — | Cria display/context/surface; fallback 3→2 |
| `make_current()` | `const` | `eglMakeCurrent` se tudo válido |
| `recreate_surface(ANativeWindow*)` | — | Destrói e recria a surface com o mesmo config |
| `set_swap_interval(int)` | `const` | `eglSwapInterval` |
| `swap()` | `const` | `eglSwapBuffers`; `bool` de sucesso |
| `clear_current()` | `const` | Solta current (surface e contexto nulos) |
| `destroy()` | — | Libera surface, contexto e display |
| `version()` | `const` | Versão efetiva (2 ou 3) |
| `display()` | `const` | `EGLDisplay` |
| `context()` | `const` | `EGLContext` |

Cópia e atribuição por cópia estão **deletadas**.

### 9.3 `Shader`

| Método | Descrição |
|---|---|
| `Shader()` / `~Shader()` | Construtor padrão / destrutor chama `destroy()` |
| `compile(vertex_src, fragment_src)` | Compila e linka fontes GLSL inline |
| `load(vertex_path, fragment_path)` | Lê os fontes de `assets/` via `Assets` |
| `use() const` | `glUseProgram(program_)` |
| `set_uniform(name, float)` | `glUniform1f` |
| `set_uniform(name, int)` | `glUniform1i` |
| `set_uniform(name, float, float)` | `glUniform2f` |
| `set_uniform(name, float, float, float)` | `glUniform3f` |
| `set_uniform(name, float, float, float, float)` | `glUniform4f` |
| `set_uniform_mat4(name, const float*)` | `glUniformMatrix4fv(..., 1, GL_FALSE, ...)` |
| `attrib(name)` | `glGetAttribLocation` (sem cache) |
| `program() const` | `GLuint` do programa |
| `destroy()` | Deleta programa e limpa cache de locations |

### 9.4 `Mesh` e `MeshData`

| Classe | Método | Descrição |
|---|---|---|
| `MeshData` | `add_triangle(a,b,c)` | 3 vértices + 3 índices `uint16_t` |
| `MeshData` | `add_quad(tl,tr,br,bl)` | Dois triângulos |
| `MeshData` | `static quad(w=1,h=1)` | Quadrado centrado, UV 0–1, branco |
| `MeshData` | `static triangle()` | Triângulo legado em NDC |
| `Mesh` | `Mesh()` / `~Mesh()` | Construtor padrão / `destroy()` |
| `Mesh` | `upload(const MeshData&)` | VBO + IBO opcional, `GL_STATIC_DRAW` |
| `Mesh` | `draw() const` | Atribs 0–3, `glDrawElements`/`glDrawArrays` |
| `Mesh` | `destroy()` | Deleta VBO e IBO |
| `Mesh` | `vertex_count() const` | Nº de vértices registrado no upload |
| `Mesh` | `set_vertex_count(GLsizei)` | Sobrescreve o contador usado por `glDrawArrays` |

Campos públicos de `MeshData`: `vertices`, `indices`, `topology`.
`SpriteVertex` existe, mas não é usado por estas APIs.

### 9.5 `Texture`

| Método | Descrição |
|---|---|
| `Texture()` / `~Texture()` | Construtor padrão / `destroy()` |
| `load(path) -> bool` | Decode via stb_image embutido; RGBA, `GL_LINEAR`, `CLAMP_TO_EDGE` |
| `bind(unit=0) const` | `glActiveTexture(GL_TEXTURE0+unit)` + `glBindTexture` |
| `destroy()` | Deleta a textura |
| `width() const` / `height() const` | Dimensões do último load |
| `id() const` | `GLuint` da textura (`0` se inexistente) |

### 9.6 `GLES2Scene` (implementa `Scene`)

| Método | Origem | Descrição |
|---|---|---|
| `GLES2Scene()` / `~GLES2Scene()` | própria | Destrutor chama `destroy()` |
| `init(ANativeWindow*)` | própria (`virtual`) | Chama `init(window, 2)` |
| `init(window, version)` | `protected` | Inicialização real; reutilizada por `GLES3Scene` |
| `set_shader(Shader*)` | própria | Override não proprietário |
| `set_mesh(Mesh*)` | própria | Override não proprietário (usado por `render()`) |
| `resize(int,int)` | `Scene` | Viewport + `mat4_ortho2d` |
| `set_clear_color(float,float,float,float=1)` | `Scene` | Cor de clear |
| `set_offset(float,float)` | própria | Matriz model da compatibilidade |
| `set_projection(const Mat4&)` | própria | Substitui a projeção |
| `set_swap_interval(int)` | própria | Repassa ao EGL |
| `draw(const MeshData&, const Material&, const float* = nullptr)` | `Scene` | Upload + `draw_mesh` |
| `draw_mesh(const Mesh&, const Material&, const float* = nullptr)` | própria | Uniforms, textura, estados, draw |
| `begin_frame()` | `Scene` | Restaura estados e faz clear |
| `present()` | `Scene` | `eglSwapBuffers` |
| `render()` | própria | Clear + triângulo/mesh em NDC + swap |
| `destroy()` | própria | Limpa cache, malhas, shader e EGL |
| `egl()` / `egl() const` | própria | Acesso ao `GLContext` |

### 9.7 `GLES3Scene`

| Método | Descrição |
|---|---|
| `using GLES2Scene::init` | Reexpõe a sobrecarga com versão |
| `init(ANativeWindow*)` | Chama `GLES2Scene::init(window, 3)` + aviso se caiu para 2 |
| `draw_instanced(const MeshData&, const Material&, const std::vector<Mat4>&)` | Loop de `draw()` com `u_model` por matriz |

---

## 10. Armadilhas e detalhes perigosos

### 10.1 State leak GL

`Mesh::draw()` liga `glEnableVertexAttribArray(0..3)` e **não desliga**.
`GLES2Scene::draw_mesh()` desliga logo depois, então o caminho normal está
coberto. Se você chamar `mesh.draw()` direto (fora da cena), desative:

```cpp
mesh.draw();
for (GLuint i = 0; i < 4; ++i) glDisableVertexAttribArray(i);
```

O mesmo vale para estados de `Material`: `depth_test`/`blend` são alterados a
cada `draw_mesh()` e só voltam ao padrão no próximo `begin_frame()` — se você
gerencia o frame sem `begin_frame()` da cena, restaure-os você mesmo.

### 10.2 Textura falha e o cache

- Caminhos de textura são cacheados por string em `textures_`.
- Um `load()` com falha **não entra** no cache (a tentativa se repete a cada
  draw) e, pior, o código chama `it->second->bind(0)` com `it == end()` —
  comportamento indefinido.
- **Mitigação segura:** valide com `Texture prova; if (prova.load(caminho))`
  antes de colocar `caminho` em `Material::texture`; nunca deixe um caminho
  inválido chegar a `draw_mesh()`.
- O cache só é limpo em `destroy()`; renomear/sobrescrever um asset em runtime
  não atualiza a textura já carregada.

### 10.3 `glGetError` só em `APKFAST_DEBUG`

Sem `APKFAST_DEBUG`, `gl_check()` não chama `glGetError()` — erros GL silenciosos
não aparecem. Compilações de debug devem definir a macro para ver os warnings
`nativeapp-gl: <operação>: glGetError=0x...`. Mesmo em debug, `gl_check()` só
**loga**; não interrompe a execução. Falhas de `glCompileShader`/link e de EGL
já lançam `GLError` independentemente da macro.

### 10.4 Duplo frame boundary: `render()` + `set_scene()`

O `App` executa, a cada frame:

```cpp
if (scene_) scene_->begin_frame();   // clear
on_draw();
if (scene_) scene_->present();       // swap
```

Se você chamar `set_scene(&cena)` **e** `cena.render()` dentro de `on_draw()`,
acontece:

1. `begin_frame()` do App → clear;
2. `render()` → `begin_frame()` → **segundo clear**;
3. `render()` → `present()` → **primeiro swap**;
4. `present()` do App → **segundo swap**.

Resultado: limpeza duplicada, swap duplicado e comportamento dependente do
driver. Escolha **um** dos estilos:

| Estilo | `set_scene()` | `on_draw()` |
|---|---|---|
| Recomendado (`nativeapp-advanced`) | sim | apenas `draw()` / `draw_mesh()` |
| Legado (`nativeapp-gl`) | não | `render()` (faz clear+swap) |

> **Observação:** chamar `render()` sem `set_scene()` é exatamente o que o
> exemplo `nativeapp-gl` faz; chamar `draw()` sem `set_scene()` não limpa a tela
> nem troca buffers — você teria que fazer `begin_frame()`/`present()` na mão.

### 10.5 NDC vs coordenadas de tela

| Contexto | Sistema de coordenadas |
|---|---|
| `draw()` depois de `resize(w,h)` | pixels: X `0..w`, Y `0..h` com origem no topo, Z `-1..1` |
| `render()` | NDC: X/Y `-1..1`, Y para cima; triângulo embutico em `-0.5..0.5` |
| `set_offset(x,y)` | NDC — converta toque com `x = tx/w*2-1`, `y = 1-ty/h*2` |
| `set_projection(mat4_perspective_gl(...))` | projeção 3D OpenGL, depth `-1..1` (cuidado com `near/far`) |

Erros comuns:

- usar `width()/2` com `render()` (a ortho2d não está ativa) → objeto fora da
  tela;
- usar `-0.5..0.5` com `draw()` e ortho2d → triângulo minúsculo no canto
  superior esquerdo;
- chamar `set_projection()` e depois `resize()` → a ortho2d sobrescreve a
  matriz customizada; chame `set_projection()` **após** o último `resize()`;
- esquecer `resize()` após `recreate_surface()` → viewport/projection
  defasados com a nova janela.

### 10.6 Outras armadilhas menores

- `Shader::use()` não é implícito nos setters; `glUniform*` atua no programa
  atual. `draw_mesh()` já chama `use()`.
- Uniform inexistente é cacheado como `-1` e falha silenciosamente.
- `set_vertex_count()` altera só o contador de `glDrawArrays`; não redimensiona
  o VBO.
- Índices são `uint16_t` (§4.3): geometrias grandes exigem múltiplas malhas.
- `set_shader()`/`set_mesh()` guardam ponteiros crus: a cena não os deleta e
  não verifica se continuam vivos.
- `SpriteVertex` não tem caminho de upload neste cabeçalho.
- `draw_instanced()` não é instancing real (§7.2): N matrizes = N `draw()`s com
  N uploads.
- `init()` é idempotente enquanto `initialized_` for `true`; `destroy()` só
  libera se `initialized_` for `true`.
