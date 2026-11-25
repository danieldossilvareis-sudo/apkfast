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
  GLES3Scene  — herda tudo e adiciona init(3) + instancing GLES3 real
```

| Camada | Papel real no código | O que **não** faz |
|---|---|---|
| `GLContext` | `eglGetDisplay`/`eglInitialize`/`eglChooseConfig`/`eglCreateContext`/`eglCreateWindowSurface`, `eglMakeCurrent`, `eglSwapBuffers`, `eglSwapInterval`, destruição EGL | Não desenha, não sabe o que é uma malha; não chama `glViewport` |
| `Shader` | Compila estágios, linka programa, fixa locations 0–3, guarda cache de uniforms, aplica `set_uniform*` | Não faz `use()` implícito nos setters; não gerencia texturas |
| `Mesh` | Faz upload de `MeshData` para VBO/IBO e emite `glDrawElements`/`glDrawArrays` | Não desativa os arrays de atributos após `draw()`; não faz instancing |
| `Texture` | Decode via stb_image embutido, `glTexImage2D`, filtros/wrap, `glActiveTexture`+`glBindTexture` | Não seta o uniform sampler; não gerencia cache de caminhos |
| `GLES2Scene` | Implementa `Scene`: inicializa GL, projection ortho2d, materiais, cache de texturas, `begin_frame`/`present`, `draw`/`draw_mesh`, `render()` legado | Não é proprietária dos ponteiros passados em `set_shader`/`set_mesh` |
| `GLES3Scene` | Pede contexto 3, cria VAO/VBO de instâncias e oferece `draw_instanced()` com matriz/cor por instância | Não cria texturas próprias; se cair para GLES2, o caminho instanciado não existe (há fallback explícito) |

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
- Na prática, `gl_check()` é chamado em `Mesh::draw()` (`"mesh_draw"`), no fim
  de `GLES2Scene::draw_mesh()` (`"draw_mesh"`) e no instancing (`"draw_instanced"`
  e `"instancing_alloc"`).

`gl_detail::context_generation()` é o outro mecanismo de segurança: um contador
que muda em `GLContext::init()`, `GLContext::destroy()` e no instante em que uma
perda de contexto é detectada. `Shader`, `Mesh` e `Texture` guardam a geração em
que seus nomes foram criados. Se a geração atual divergir, `use()`/`draw()`/
`bind()` são no-ops e `destroy()` não chama `glDelete*` sobre nomes mortos. Isso
é o equivalente GLES do "descartar sem usar handles" feito pelo Vulkan em
`VulkanScene::handle_device_loss()`.

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
    bool make_current() const;           // false também quando o contexto foi perdido
    void recreate_surface(ANativeWindow* window);
    void set_swap_interval(int interval) const;
    bool swap() const;
    void clear_current() const;
    void destroy();

    bool context_lost() const;           // EGL_CONTEXT_LOST detectado
    bool device_lost() const;            // alias, mesmo contrato do Vulkan
    bool needs_asset_reload() const;     // true até init()/recreate() bem-sucedido

    int           version() const;
    int           requested_version() const;
    EGLDisplay    display() const;
    EGLContext    context() const;
    ANativeWindow* window() const;
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
são válidos e nenhuma perda foi marcada:

```cpp
eglMakeCurrent(display_, surface_, surface_, context_);  // draw = read = surface
```

Ele devolve `EGL_TRUE == ok`. Quando a chamada falha, o código consulta
`eglGetError()`: se o valor for `EGL_CONTEXT_LOST` (`0x300E`), a perda é marcada,
um erro é logado em `nativeapp-gl` e `gl_detail::context_generation()` avança.
`swap()` faz o mesmo com `eglSwapBuffers` — os dois são os pontos onde o driver
reporta a perda. Falhas por surface inválida (`EGL_BAD_SURFACE` etc.) continuam
sendo apenas avisos: elas são tratadas por `recreate_surface()`, não como device
loss.

Depois da marcação, `make_current()`, `swap()` e `set_swap_interval()` viram
no-ops até `init()` (ou `GLES2Scene::recreate()`).

`clear_current()` chama `eglMakeCurrent(display_, EGL_NO_SURFACE,
EGL_NO_SURFACE, EGL_NO_CONTEXT)` (se houver display). `recreate_surface()` usa
isso internamente antes de destruir a surface antiga e também marca perda se o
erro for `EGL_CONTEXT_LOST`.

> **Observação:** as cenas não chamam `egl_.make_current()` diretamente: usam
> `ensure_current()`, que checa a perda antes e depois do `eglMakeCurrent`.
> Isso reconecta o contexto após uma recriação de surface ou após outra thread
> ter feito current, sem deixar um draw escapar com o contexto morto.

### 2.3 `recreate_surface(window)`

1. Sai imediatamente se o contexto foi perdido, ou se `display_`/`config_`
   são inválidos.
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
| `set_swap_interval(int) const` | `eglSwapInterval(display_, interval)` se houver display e contexto íntegro (`0` desliga vsync; `1` sincroniza) |
| `swap() const` | `eglSwapBuffers(display_, surface_) == EGL_TRUE`; retorna `false` se não há display/surface ou se a perda foi marcada |
| `destroy()` | Destrói surface, contexto, chama `eglTerminate` e zera display/config/window. Após perda, **não** chama `eglDestroySurface`/`eglDestroyContext`: termina o display e descarta os handles com aviso explícito |
| `context_lost()` / `device_lost()` | `true` desde que `eglGetError() == EGL_CONTEXT_LOST` foi visto até a próxima inicialização |
| `needs_asset_reload()` | `true` no mesmo intervalo; o app deve recarregar recursos próprios antes/ao recuperar |
| `version()`, `requested_version()`, `display()`, `context()`, `window()` | Estado atual; `version()` reflete o fallback, `requested_version()` guarda o pedido original |

### 2.5 Perda de contexto e recuperação

`EGL_CONTEXT_LOST` (`0x300E`) é detectado em `make_current()`, `swap()`,
`recreate_surface()`, `set_swap_interval()` e `clear_current()`. O resultado é
sempre o mesmo:

1. log `ANDROID_LOG_ERROR` com tag `nativeapp-gl` e nome da operação;
2. `context_lost_ = true` e `needs_asset_reload_ = true`;
3. `gl_detail::bump_context_generation()` — todos os nomes GL passam a ser
   considerados inválidos imediatamente;
4. nenhum `glDelete*` e nenhum `eglDestroySurface`/`eglDestroyContext` é chamado
   sobre o contexto morto.

Para recuperar, chame `GLContext::init()` novamente (com a mesma janela e
versão desejada). `init()` descarta o estado antigo com `eglTerminate`,
recria display/config/context/surface, zera as duas flags e avança a geração.
O mesmo vale para `GLES2Scene::recreate()` (§6.8).

```cpp
if (ctx.context_lost()) {
    const bool reload = ctx.needs_asset_reload();  // leia ANTES de init()
    ctx.init(janela, 3);                           // geração nova
    // recarregue Shader/Texture/Mesh próprias se reload == true
}
```

> **Atenção:** `EGL_CONTEXT_LOST` não é a mesma coisa que surface destruída.
> `EGL_BAD_SURFACE`/`EGL_BAD_MATCH` durante `on_surface_destroyed()` apenas
> geram warning; use `recreate_surface()`/`init()` normalmente nesse caso.
> O mecanismo de device loss só dispara com o erro explícito de perda.

### 2.6 Por que **não** chamar `resize()` dentro de `init()`

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
    GLuint program() const;              // 0 se a geração de contexto mudou
    bool   valid() const;                // programa vivo na geração atual
    void   release_without_gl();         // descarta o nome sem glDeleteProgram
    void   destroy();
};
```

> **Device loss:** `use()` e `set_uniform*` viram no-op e `program()` devolve
> `0` se o programa pertence a outra geração de contexto. `destroy()` só chama
> `glDeleteProgram` quando a geração confere. `release_without_gl()` é usado
> pela cena no instante da perda; um posterior `compile()` reconstrói o
> programa sem tocar no nome antigo.

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

    bool valid() const;                  // geração de contexto conferida
    void release_without_gl();           // descarta VBO/IBO sem glDeleteBuffers

    GLsizei vertex_count() const;
    void set_vertex_count(GLsizei count);
};
```

`upload()` começa chamando `refresh_generation()`: se o contexto mudou, os
nomes antigos de VBO/IBO são zerados sem `glDeleteBuffers` e novos nomes são
gerados. `draw()` retorna imediatamente quando a geração diverge; `destroy()`
também não apaga nomes de outra geração. Isso cobre tanto a perda detectada
quanto uma troca explícita de `GLContext`.

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

`Mesh` **não** tem `draw_instanced()` público. O instancing está em
`GLES3Scene` (§7): lá a malha é reaproveitada via amizade de classe (os buffers
da `Mesh` são ligados diretamente) e o desenho sai em **um**
`glDrawElementsInstanced`/`glDrawArraysInstanced`, com matriz/cor vindas de um
VBO com divisor de atributo 1.

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

    bool valid() const;                  // textura viva na geração atual
    void release_without_gl();           // descarta o nome sem glDeleteTextures
    void destroy();

    int    width() const;
    int    height() const;
    GLuint id() const;
};
```

> **Device loss:** `bind()` já checava `generation_` e continua sendo no-op com
> nome morto. `load()`/`load_rgba()` chamam `refresh_generation()` antes do
> upload; `destroy()` e o próprio `upload_rgba()` só chamam `glDeleteTextures`
> se a geração ainda é a mesma. Isso evita apagar um nome que pode pertencer a
> outro objeto no contexto recuperado.

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
    void recreate(ANativeWindow* window = nullptr, int client_version = -1);

    bool context_lost() const;
    bool device_lost() const;            // alias do contrato Vulkan
    bool needs_asset_reload() const;     // true até init()/recreate()

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

1. Guarda `window_` e `requested_client_version_` para `recreate()`.
2. Se uma perda está pendente, chama `handle_context_loss()` (invalida sem
   `glDelete*`). Se `initialized_` já é `true`, retorna (idempotente).
3. `egl_.init(window, client_version)` — descarta contexto perdido e recria EGL.
4. `glEnable(GL_DEPTH_TEST)`, `glEnable(GL_BLEND)`,
   `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`.
5. `glClearColor(clear_...)` — padrão `{0.08f, 0.08f, 0.12f, 1.0f}`.
6. `shader_.compile(kVertexShader, kFragmentShader)` — shaders embutidos.
7. `mesh_.upload(default_mesh())` — `MeshData::triangle()`.
8. Tenta `resize()` com `ANativeWindow_getWidth/Height` se ambos > 0 (§2.6).
9. Em recuperação: recarrega as texturas do cache e reenvia o atlas da fonte
   (se `load_font()` já tiver sido chamado), restaura viewport com
   `width_`/`height_` e limpa `needs_asset_reload()`.
10. Marca `initialized_ = true` e loga o aviso de contexto reinicializado.

> **Observação:** se `init()` lançar `GLError`, ele destrói EGL, limpa os
> objetos parciais e deixa a cena pronta para nova tentativa. Recursos
> externos (seu `Shader`, `Mesh`, `Texture` fora da cena) continuam com a
> geração antiga e devem ser reenviados.

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

1. Retorna se a cena não foi inicializada ou `ensure_current()` falhar.
2. `temporary_mesh_.upload(data)`.
3. Chama `draw_mesh(temporary_mesh_, material, model)`.

`draw_mesh()` (o coração do desenho):

1. Garante contexto atual (`ensure_current()`); escolhe `shader_override_` se
   definido, senão `shader_`. Sai se `program() == 0` (geração divergente) e
   chama `use()`.
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

> **Atenção:** `cached_texture()` (protegido, usado por `draw_mesh()` e pelo
> caminho instanciado) retorna `nullptr` quando o `load()` falha, sem inserir a
> textura no cache. Nesse caso `u_use_texture` vira `0` e nenhum `bind()` é
> feito: o warning `Textura nao carregada: <path>` continua sendo emitido, mas
> não há mais dereferência de `end()`. Validar com `Texture::load()` antes de
> preencher `Material::texture` ainda evita tentativas repetidas por frame.

> **Observação:** `temporary_mesh_` é **um único** objeto reaproveitado. Cada
> `draw()` faz novo `glBufferData` — ótimo para geometria dinâmica, ruim para
> dezenas de objetos estáticos. Para malhas reutilizáveis, faça
> `mesh.upload(data)` uma vez e chame `draw_mesh(mesh, material, model)`.

### 6.5 `begin_frame()` / `present()` — boundary de frame

```cpp
void begin_frame() override {
    if (!initialized_ || !ensure_current()) return;
    // Restaura defaults para que um material anterior não vaze estado.
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glClearColor(clear_[0], clear_[1], clear_[2], clear_[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void present() override {
    if (!initialized_ || !ensure_current()) return;
    if (!egl_.swap()) {
        if (egl_.context_lost()) handle_context_loss();
        else __android_log_print(ANDROID_LOG_WARN, "nativeapp-gl",
                                 "eglSwapBuffers falhou");
    }
}
```

`begin_frame()` restaura depth/blend/blendFunc/clearColor e faz o clear. Isso
é o que impede que um `Material` com `depth_test=false` ou `blend=false` do
frame anterior vaze para o próximo. `present()` troca os buffers e, se
`eglSwapBuffers` devolver `EGL_CONTEXT_LOST`, marca a perda com `handle_context_loss()`.

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

Se a perda foi marcada, chama `handle_context_loss()` (descarta nomes sem
`glDelete*`) e `egl_.destroy()` sem tocar em EGL inválido. Caso contrário, só
age se `initialized_`: garante current, limpa o cache de texturas, destrói
`temporary_mesh_`, `mesh_`, `shader_` e o texto, chama `egl_.destroy()` e zera
`initialized_`.

> **Observação:** ponteiros registrados com `set_shader()`/`set_mesh()` não são
> deletados — a vida útil deles é de quem os criou.

### 6.8 Device loss: detectar, parar e recuperar

A cena detecta perda por dois caminhos:

1. `GLContext` marca `EGL_CONTEXT_LOST` em `make_current()`/`swap()` (ou nas
   outras chamadas EGL listadas em §2.5);
2. `GLES2Scene::ensure_current()` compara `scene_generation_` com
   `gl_detail::context_generation()`. Se divergirem — por exemplo porque o app
   chamou `egl().init()` diretamente — a cena também entra em modo perdido.

Em ambos os casos, `handle_context_loss()`:

- loga uma vez com `ANDROID_LOG_ERROR`;
- chama `invalidate_resources_after_loss()`: `release_without_gl()` em
  `shader_`, `text_shader_`, `mesh_`, `temporary_mesh_` e nas texturas do cache;
  zera `font_texture_` e `text_vbo_`; preserva `font_` (CPU-side) para reupload;
- zera `initialized_`, então `draw()`, `begin_frame()`, `present()`,
  `draw_text()` e `resize()` não chamam GL até a recuperação;
- mantém `needs_asset_reload() == true` até a próxima inicialização.

`GLES3Scene` estende o hook para liberar também `instance_vbo_`,
`instance_vao_` e `instanced_shader_` sem `glDelete*`.

Recuperação explícita:

```cpp
void on_update(float dt) override {
    if (scene_.context_lost()) {
        const bool reload = scene_.needs_asset_reload();  // leia antes
        scene_.recreate();     // reusa janela/versão do init() anterior
        // ou: scene_.init(window()); com a janela atual
        if (reload) recarregar_meus_shaders_e_malhas();
    }
}
```

`recreate()` chama `destroy()` (se ainda initialized) ou `handle_context_loss()`
e depois `init()` com a janela/versão salvas. `init()` reconstrói shader
embutido, mesh padrão, texturas cacheadas e atlas de fonte; `needs_asset_reload()`
volta a `false`. Recursos **externos** (`set_shader()`, `set_mesh()`, `Texture`
própria, `Shader` carregado de `assets/`) precisam de `compile()`/`upload()`/
`load()` de novo — seus nomes antigos não são reaproveitados.

> **Atenção:** não tente "consertar" uma perda chamando apenas
> `recreate_surface()`. Ela lida com janela/surface, não com contexto perdido:
> use `init()`/`recreate()` para gerar uma geração nova de nomes GL.

---

## 7. `GLES3Scene` — Instancing GLES3 real

```cpp
class GLES3Scene : public GLES2Scene {
public:
    /// Matriz model (column-major) + cor RGBA por instância.
    struct Instance {
        Mat4 model = mat4_identity();
        std::array<float, 4> color{{1.0f, 1.0f, 1.0f, 1.0f}};
    };
    static_assert(sizeof(Instance) == 80);  // 4 colunas (64 B) + RGBA (16 B)

    using GLES2Scene::init;                    // expõe init(window, version)
    void init(ANativeWindow* window) override; // pede versão 3
    ~GLES3Scene() override;                    // destroy() próprio + da base
    void destroy() override;

    // Caminho novo: UM glDraw*Instanced, matrizes/cores vindas de VBO.
    bool draw_instanced(const Mesh& mesh, const Material& material,
                        const std::vector<Instance>& instances);
    bool draw_instanced(const MeshData& data, const Material& material,
                        const std::vector<Instance>& instances);

    // Compatibilidade: assinatura antiga preservada (cor branca por instância).
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

Diferença em relação a `GLES2Scene`: o argumento `3` passado ao `GLContext`,
que tenta o config ES3 e aplica o fallback descrito em §2.1. O `using
GLES2Scene::init;` reexpõe `init(window, client_version)`; `init(window, 2)`
pede GLES2 explicitamente. `destroy()` é `virtual` para que a limpeza do VAO,
do VBO de instâncias e do shader GLES3 também aconteça via `Scene*`.

`context_lost()`, `device_lost()`, `needs_asset_reload()` e `recreate()` são
herdados de `GLES2Scene`. `GLES3Scene` sobrescreve apenas
`invalidate_resources_after_loss()` para descartar `instance_vbo_`,
`instance_vao_` e `instanced_shader_` sem `glDelete*`. Um `instance_generation_`
próximo guarda a geração desses nomes: se divergir, `draw_instanced_impl()`
devolve `false` e um novo VAO/VBO é gerado no próximo desenho válido.

> **Atenção (builder):** `apkfast/builder.py` detecta GLES pelo **token
> `GLES2Scene` no texto do `.cpp`** (comentários são removidos) para adicionar
> `-lEGL -lGLESv2`. Um app que só menciona `GLES3Scene` linka sem EGL e falha no
> `dlopen` com `cannot locate symbol "eglMakeCurrent"`. O exemplo avançado mantém
> o token com um `static_assert(std::is_base_of<GLES2Scene, GLES3Scene>...)`.
> Em outros projetos, use o mesmo recurso ou `--ldflags "-lEGL -lGLESv2"`.

### 7.2 API de instâncias e o caminho real

`Instance` é uma struct de layout fixo: `Mat4` (column-major, 64 bytes) seguida
de RGBA (16 bytes). A cor final é:

```text
texture (se houver) × cor do vértice × Material.color × Instance.color
```

As sobrecargas com `std::vector<Instance>` retornam `bool`:

| Valor | Significado |
|---|---|
| `true` | Houve **um** `glDrawElementsInstanced`/`glDrawArraysInstanced` real (GLES3) |
| `false` | Nada desenhado (lista vazia), ou contexto GLES2 / shader GLES3 indisponível → fallback documentado em §7.3 |

A sobrecarga com `Mesh` reaproveita geometria já na GPU (upload feito uma vez
pelo chamador). A sobrecarga com `MeshData` faz **um único** `upload()` por
chamada e destrói a `Mesh` local no retorno. Para muitos frames com a mesma
geometria, prefira `Mesh`.

Passos do caminho GLES3 (`draw_instanced_impl`):

1. Sai se não há instâncias, se a `Mesh` não é válida na geração atual ou se
   `egl().version() < 3`.
2. `ensure_current()` e compilação preguiçosa de um shader `#version 300 es`
   próprio (falha de compilação vira warning + fallback, nunca exceção).
3. Cria um VAO e um VBO de instâncias (ambos reaproveitados entre frames).
4. Uniforms: `u_projection` (vem de `resize()`/`set_projection()`), `u_color`
   (`Material`) e, se houver textura, `u_texture`/`u_use_texture`.
5. Atributos **por vértice** locations 0–3 (posição/normal/UV/cor) com
   `glVertexAttribDivisor(loc, 0)`, apontando para o VBO da `Mesh`.
6. Atributos **por instância**: locations 4–7 são as quatro colunas da `Mat4`
   (`glVertexAttribPointer(4+i, 4, GL_FLOAT, ..., i*16)`), location 8 é a cor;
   todos com `glVertexAttribDivisor(loc, 1)` e stride `sizeof(Instance) = 80`.
7. `glDrawElementsInstanced(...)` quando há IBO válido, senão
   `glDrawArraysInstanced(...)` — **uma chamada para N instâncias**.
8. Restaura estado: `glVertexAttribDivisor(0..8, 0)`,
   `glDisableVertexAttribArray(0..8)` e `glBindVertexArray(0)`, para não vazar
   para `draw_mesh()`.

> **Observação:** o shader usa `layout(location = ...)` do GLSL ES 3.00, então
> não depende de `glBindAttribLocation` extra. `Shader::link()` continua fixando
> 0–3 para os nomes `a_position`/`a_normal`/`a_uv`/`a_color`.

### 7.3 Compatibilidade e fallback honesto

A assinatura antiga continua existindo **exatamente** como antes:

```cpp
void draw_instanced(const MeshData& data, const Material& material,
                    const std::vector<Mat4>& models);
```

Ela converte cada `Mat4` em `Instance` com cor branca e chama a nova sobrecarga.
Como retorna `void`, não informa se houve fallback; use a sobrecarga com
`std::vector<Instance>` quando isso importar.

| Aspecto | Loop antigo (até v2.7.3) | Instanciamento real (agora) |
|---|---|---|
| Chamadas de draw | N (`draw()` por matriz) | **1** (`glDrawElementsInstanced`/`glDrawArraysInstanced`) |
| Upload de geometria | N (`temporary_mesh_` refaz `glBufferData`) | 1 por chamada (`MeshData`) ou 0 (`Mesh` já na GPU) |
| Matriz por instância | uniform `u_model` por draw | atributo com divisor 1 (locations 4–7) |
| Cor por instância | impossível sem outro uniform/draw | atributo com divisor 1 (location 8) |
| VAO / divisor | inexistentes | `glGenVertexArrays` + `glVertexAttribDivisor` |
| Contexto GLES2 | funcionava (era a única opção) | fallback explícito para o loop, com warning |
| `models` vazio | retorno imediato | `false`, nenhum draw |

No fallback (GLES2 ou shader GLES3 que não compilou), a cena loga
`nativeapp-gl: draw_instanced: GLES%d sem instancing real; fallback com N
draws` e usa `draw_mesh()`/`draw()` com `Material.color` multiplicado pela cor
da instância. Assim o exemplo continua desenhando sem FATAL, mas o `bool`
deixa claro que **não** houve instanciamento.

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

// (b) Instanciamento real via GLES3Scene (um draw call).
GLES3Scene c3;
c3.init(window());              // tenta 3, faz fallback para 2
std::vector<GLES3Scene::Instance> instances;
instances.push_back({mat4_translate(10, 0, 0), {1.0f, 0.2f, 0.2f, 1.0f}});
instances.push_back({mat4_translate(20, 0, 0), {0.2f, 0.2f, 1.0f, 1.0f}});
const bool real = c3.draw_instanced(custom_, material_, instances);
// Assinatura antiga (Mat4, cor branca) continua válida:
c3.draw_instanced(custom_, material_, {mat4_translate(30, 0, 0)});
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
| `init(ANativeWindow*, int = 2)` | — | Cria display/context/surface; fallback 3→2; recupera perda |
| `make_current()` | `const` | `eglMakeCurrent` se tudo válido; detecta `EGL_CONTEXT_LOST` |
| `recreate_surface(ANativeWindow*)` | — | Destrói e recria a surface com o mesmo config |
| `set_swap_interval(int)` | `const` | `eglSwapInterval` |
| `swap()` | `const` | `eglSwapBuffers`; detecta `EGL_CONTEXT_LOST` |
| `clear_current()` | `const` | Solta current (surface e contexto nulos) |
| `destroy()` | — | Libera EGL; após perda só descarta handles |
| `context_lost()` / `device_lost()` | `const` | Estado de perda de contexto |
| `needs_asset_reload()` | `const` | Recursos próprios do app precisam de reload |
| `version()` / `requested_version()` | `const` | Versão efetiva e versão pedida |
| `display()` / `context()` / `window()` | `const` | Estado atual do EGL |

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
| `program() const` | `GLuint` do programa, ou `0` se a geração divergiu |
| `valid() const` | Programa vivo na geração atual |
| `release_without_gl()` | Descarta nome sem `glDeleteProgram` |
| `destroy()` | Deleta programa (geração atual) e limpa cache de locations |

### 9.4 `Mesh` e `MeshData`

| Classe | Método | Descrição |
|---|---|---|
| `MeshData` | `add_triangle(a,b,c)` | 3 vértices + 3 índices `uint16_t` |
| `MeshData` | `add_quad(tl,tr,br,bl)` | Dois triângulos |
| `MeshData` | `static quad(w=1,h=1)` | Quadrado centrado, UV 0–1, branco |
| `MeshData` | `static triangle()` | Triângulo legado em NDC |
| `Mesh` | `Mesh()` / `~Mesh()` | Construtor padrão / `destroy()` |
| `Mesh` | `upload(const MeshData&)` | VBO + IBO opcional, `GL_STATIC_DRAW` |
| `Mesh` | `draw() const` | Atribs 0–3, `glDrawElements`/`glDrawArrays`; no-op se geração divergiu |
| `Mesh` | `valid() const` / `release_without_gl()` | Checagem e descarte seguro de nomes |
| `Mesh` | `destroy()` | Deleta VBO e IBO da geração atual |
| `Mesh` | `vertex_count() const` | Nº de vértices registrado no upload |
| `Mesh` | `set_vertex_count(GLsizei)` | Sobrescreve o contador usado por `glDrawArrays` |

Campos públicos de `MeshData`: `vertices`, `indices`, `topology`.
`SpriteVertex` existe, mas não é usado por estas APIs.

### 9.5 `Texture`

| Método | Descrição |
|---|---|
| `Texture()` / `~Texture()` | Construtor padrão / `destroy()` |
| `load(path) -> bool` | Decode via stb_image embutido; RGBA, `GL_LINEAR`, `CLAMP_TO_EDGE` |
| `bind(unit=0) const` | `glActiveTexture(GL_TEXTURE0+unit)` + `glBindTexture`; no-op se geração divergiu |
| `valid() const` / `release_without_gl()` | Checagem e descarte seguro do nome |
| `destroy()` | Deleta a textura da geração atual |
| `width() const` / `height() const` | Dimensões do último load |
| `id() const` | `GLuint` da textura na geração atual (`0` se inexistente ou se o contexto mudou) |

### 9.6 `GLES2Scene` (implementa `Scene`)

| Método | Origem | Descrição |
|---|---|---|
| `GLES2Scene()` / `~GLES2Scene()` | própria | Destrutor chama `destroy()` |
| `init(ANativeWindow*)` | própria (`virtual`) | Chama `init(window, 2)`; recupera perda pendente |
| `init(window, version)` | `protected` | Inicialização real; reutilizada por `GLES3Scene` |
| `recreate(window=nullptr, version=-1)` | própria | `destroy()`/`handle_context_loss()` + `init()` |
| `context_lost()` / `device_lost()` | própria | Estado de perda (alias do VK) |
| `needs_asset_reload()` | própria | `true` até inicialização bem-sucedida |
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

`destroy()` é `virtual` (a limpeza da `GLES3Scene` acontece antes da base).
`init(window, version)`, `projection()` e `cached_texture()` são protegidos.

### 9.7 `GLES3Scene`

| Método | Descrição |
|---|---|
| `Instance` | Struct pública: `Mat4 model` + `std::array<float,4> color` (80 bytes) |
| `using GLES2Scene::init` | Reexpõe a sobrecarga com versão |
| `init(ANativeWindow*)` | Chama `GLES2Scene::init(window, 3)` + aviso se caiu para 2 |
| `~GLES3Scene()` / `destroy()` | Liberam VAO, VBO de instâncias e shader antes da base (`destroy()` é virtual; sem `glDelete*` após perda) |
| Herdadas de `GLES2Scene` | `context_lost()`, `device_lost()`, `needs_asset_reload()`, `recreate()` |
| `draw_instanced(const Mesh&, const Material&, const std::vector<Instance>&) -> bool` | 1 `glDraw*Instanced`; `false` no fallback e com geração divergente |
| `draw_instanced(const MeshData&, const Material&, const std::vector<Instance>&) -> bool` | Upload único + 1 `glDraw*Instanced`; `false` no fallback |
| `draw_instanced(const MeshData&, const Material&, const std::vector<Mat4>&)` | Assinatura antiga preservada; converte para `Instance` com cor branca |

Membros protegidos acrescentados em `GLES2Scene` para a subclasse:
`const Mat4& projection() const` e `Texture* cached_texture(const std::string&)`.
`Mesh` declara `GLES3Scene` como `friend` para reaproveitar VBO/IBO sem um
`draw()` normal.

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

O caminho instanciado (§7.2) liga temporariamente os atributos 0–8 com divisor
1 em 4–8 e, ao final, zera os divisores, desliga os arrays e desfaz o
`glBindVertexArray`: chamar `draw_mesh()` logo depois é seguro.

O mesmo vale para estados de `Material`: `depth_test`/`blend` são alterados a
cada `draw_mesh()` e só voltam ao padrão no próximo `begin_frame()` — se você
gerencia o frame sem `begin_frame()` da cena, restaure-os você mesmo.

### 10.2 Textura falha e o cache

- Caminhos de textura são cacheados por string em `textures_`.
- `cached_texture()` centraliza o cache para `draw_mesh()` e para o caminho
  instanciado. Um `load()` com falha **não entra** no cache, loga o warning e
  devolve `nullptr`; `u_use_texture` vira `0` e nenhum `bind()` é chamado.
- A tentativa se repete a cada draw enquanto o caminho for inválido: valide com
  `Texture prova; if (prova.load(caminho))` antes de preencher
  `Material::texture` se isso importar.
- O cache só é limpo em `destroy()`; renomear/sobrescrever um asset em runtime
  não atualiza a textura já carregada. Em recuperação de device loss,
  `init()`/`recreate()` recarrega cada caminho do cache automaticamente.

### 10.3 `glGetError` só em `APKFAST_DEBUG`

Sem `APKFAST_DEBUG`, `gl_check()` não chama `glGetError()` — erros GL silenciosos
não aparecem. Compilações de debug devem definir a macro para ver os warnings
`nativeapp-gl: <operação>: glGetError=0x...`. Mesmo em debug, `gl_check()` só
**loga**; não interrompe a execução. Falhas de `glCompileShader`/link e de EGL
já lançam `GLError` independentemente da macro. A detecção de device loss,
diferente de `gl_check()`, **não** depende de `APKFAST_DEBUG`: `eglGetError()`
é consultado nas chamadas EGL críticas mesmo em release.

### 10.4 Device loss não é `eglSwapBuffers falhou`

Um `eglSwapBuffers` que retorna `false` com `EGL_BAD_SURFACE` é problema de
janela/surface, não perda de contexto — gera apenas warning e a cena continua.
Somente `EGL_CONTEXT_LOST` (`0x300E`) dispara `context_lost()`/
`needs_asset_reload()`. Não tente reanimar a cena com `make_current()` nesse
estado: ele é no-op até `init()`/`recreate()` (§6.8).

### 10.5 Duplo frame boundary: `render()` + `set_scene()`

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

### 10.6 NDC vs coordenadas de tela

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

### 10.7 Outras armadilhas menores

- `Shader::use()` não é implícito nos setters; `glUniform*` atua no programa
  atual. `draw_mesh()` já chama `use()`.
- Uniform inexistente é cacheado como `-1` e falha silenciosamente.
- `set_vertex_count()` altera só o contador de `glDrawArrays`; não redimensiona
  o VBO.
- Índices são `uint16_t` (§4.3): geometrias grandes exigem múltiplas malhas.
- `set_shader()`/`set_mesh()` guardam ponteiros crus: a cena não os deleta e
  não verifica se continuam vivos.
- `SpriteVertex` não tem caminho de upload neste cabeçalho.
- `draw_instanced()` só é real em contexto GLES3 (§7): com fallback para GLES2
  ele vira N `draw()`s e avisa no log; use o retorno `bool` para detectar isso.
- `init()` é idempotente enquanto `initialized_` for `true`; `destroy()` só
  libera se `initialized_` for `true`.
