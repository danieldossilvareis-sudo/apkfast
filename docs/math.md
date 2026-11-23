# Matemática 3D (`nativeapp::math.hpp`)

> Guia completo da biblioteca matemática mínima do apkfast.
>
> **Fonte consultada:** `include/nativeapp/math.hpp` (tipos, todas as funções e
> suas implementações). Onde útil, são citados usos reais em
> `include/nativeapp/gl.hpp` e a assinatura de `Scene::draw()` em
> `include/nativeapp/graphics_types.hpp`.
>
> **Regra deste documento:** só está aqui o que existe no código atual. Todas
> as 12 funções do header estão documentadas; não há operadores, classes
> `Matrix`/`Vector` nem funções extras.

## Índice

1. [Convenções: column-major, `Mat4` e `Vec3`](#1-convenções-column-major-mat4-e-vec3)
2. [Tabela completa de funções](#2-tabela-completa-de-funções)
3. [Identidade e multiplicação (`mat4_identity`, `mat4_multiply`)](#3-identidade-e-multiplicação-mat4_identity-mat4_multiply)
4. [Transformações affine (`mat4_translate`, `mat4_scale`, rotações)](#4-transformações-affine-mat4_translate-mat4_scale-rotações)
5. [Projeções (`mat4_ortho`, `mat4_ortho2d`, `mat4_perspective`, `mat4_perspective_gl`)](#5-projeções-mat4_ortho-mat4_ortho2d-mat4_perspective-mat4_perspective_gl)
6. [Câmera (`mat4_look_at`)](#6-câmera-mat4_look_at)
7. [Composição: T × R × S e MVP](#7-composição-t--r--s-e-mvp)
8. [Quando usar `perspective` ou `perspective_gl`](#8-quando-usar-perspective-ou-perspective_gl)
9. [Armadilhas](#9-armadilhas)
10. [Referência rápida](#10-referência-rápida)

---

## 1. Convenções: column-major, `Mat4` e `Vec3`

O header define apenas dois aliases:

```cpp
using Mat4 = std::array<float, 16>;
using Vec3 = std::array<float, 3>;
```

| Tipo | Definição | Observações |
|---|---|---|
| `Mat4` | `std::array<float, 16>` | 16 floats em **column-major**: o índice é `coluna * 4 + linha`. |
| `Vec3` | `std::array<float, 3>` | Tripleto `{x, y, z}`. Usado como `eye`/`center`/`up` em `mat4_look_at`. |

Mapa de índices (o que está na memória e o que cada posição significa):

```
             coluna 0   coluna 1   coluna 2   coluna 3
linha 0         0          4          8         12
linha 1         1          5          9         13
linha 2         2          6         10         14
linha 3         3          7         11         15
```

Conferindo com `mat4_identity()`:

```cpp
return Mat4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
//           col0     col1     col2     col3
```

Os elementos 0, 5, 10 e 15 são o `1` da diagonal, como esperado.

Consequências práticas do column-major:

* **Translação** vive nos índices `12`, `13`, `14` (terceira coluna, linhas
  0/1/2).
* Um vetor coluna é transformado por `M * v`; a matriz resultante de
  `mat4_multiply(a, b)` aplica `b` **primeiro** e `a` depois.
* Ao enviar para GLSL com `glUniformMatrix4fv`, o código de `Shader` usa
  `GL_FALSE` como parâmetro de transposição (`set_uniform_mat4`), justamente
  porque os dados já estão column-major.
* `Mat4` e `Vec3` são `std::array`: têm semântica de valor (`Mat4 b = a;`
  copia), comparáveis com `==`, indexáveis com `[]` e convertíveis para ponteiro
  com `.data()`.

**O que NÃO existe no header:** sobrecargas de `operator*`, `operator+`,
`operator[]` extras, funções para transformar `Vec3`, inversão de matriz,
transposição, quaternions, prow-stage `normal_matrix`, `Mat3`, `Vec2` ou
`Vec4`. As funções são `inline`, livres, dentro de `namespace nativeapp`, e
operam somente sobre `Mat4`/`Vec3` e floats.

---

## 2. Tabela completa de funções

| Função | Parâmetros | O que produz | Observações do código |
|---|---|---|---|
| `mat4_identity()` | nenhum | Matriz identidade | Literal `Mat4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}` |
| `mat4_multiply(a, b)` | `const Mat4& a`, `const Mat4& b` | `a * b` (composição) | `out[col*4+row] = Σ_k a[k*4+row] * b[col*4+k]` |
| `mat4_translate(x, y, z)` | 3 floats | Translação | Escreve `m[12]=x`, `m[13]=y`, `m[14]=z` sobre a identidade |
| `mat4_scale(x, y, z)` | 3 floats | Escala | Escreve `m[0]=x`, `m[5]=y`, `m[10]=z` sobre a identidade |
| `mat4_rotate_x(angle)` | `float angle` | Rotação em torno de X | `cos`/`sin` em `m[5], m[6], m[9], m[10]` |
| `mat4_rotate_y(angle)` | `float angle` | Rotação em torno de Y | `cos`/`sin` em `m[0], m[2], m[8], m[10]` |
| `mat4_rotate_z(angle)` | `float angle` | Rotação em torno de Z | `cos`/`sin` em `m[0], m[1], m[4], m[5]` |
| `mat4_ortho(l, r, b, t, near, far)` | 4 obrigatórios + 2 com default | Projeção ortográfica | Defaults: `near = -1.0f`, `far = 1.0f` |
| `mat4_ortho2d(width, height)` | 2 floats | Ortográfica 2D | Chama `mat4_ortho(0, width, height, 0, -1, 1)` |
| `mat4_perspective(fovy_rad, aspect, near, far)` | 4 floats | Perspectiva com profundidade **0..1** (Vulkan-style) | `m[10]=far/(near-far)`, `m[14]=(far*near)/(near-far)` |
| `mat4_perspective_gl(fovy_rad, aspect, near, far)` | 4 floats | Perspectiva com profundidade **-1..1** (OpenGL-style) | `m[10]=(far+near)/(near-far)`, `m[14]=(2*far*near)/(near-far)` |
| `mat4_look_at(eye, center, up)` | `const Vec3&`, `const Vec3&`, `const Vec3&` | Matriz de visão | Normaliza `z` e `x`, com fallbacks para casos degenerados |

Todas retornam `Mat4` **por valor** e são funções `inline`.

---

## 3. Identidade e multiplicação (`mat4_identity`, `mat4_multiply`)

### `mat4_identity()`

```cpp
inline Mat4 mat4_identity() {
    return Mat4{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}
```

É usada como base por `mat4_translate`, `mat4_scale`, todas as rotações,
`mat4_ortho` e `mat4_look_at`. Em `gl.hpp`, `GLES2Scene` usa identidade como
`model` quando `draw()` recebe `model == nullptr`, e como projeção no método
`render()` de compatibilidade.

### `mat4_multiply(a, b)`

```cpp
inline Mat4 mat4_multiply(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
    return out;
}
```

É a multiplicação de matrizes convencional `A × B`, implementada diretamente
sobre o armazenamento column-major:

* `a[k * 4 + row]` → elemento `a(row, k)`.
* `b[col * 4 + k]` → elemento `b(k, col)`.
* `out[col * 4 + row]` → elemento `out(row, col)`.

**Ordem:** com vetores coluna, `mat4_multiply(a, b) * v` equivale a
`a * (b * v)`. O operando da **direita** (`b`) é aplicado primeiro. Portanto,
`mat4_multiply(A, B) != mat4_multiply(B, A)` em geral.

```cpp
using namespace nativeapp;

const Mat4 t = mat4_translate(1.0f, 2.0f, 3.0f);
const Mat4 s = mat4_scale(2.0f, 2.0f, 2.0f);

const Mat4 ts = mat4_multiply(t, s);  // T*S: escala primeiro, translada depois
const Mat4 st = mat4_multiply(s, t);  // S*T: translada primeiro, escala depois
// ts e st são diferentes.
```

Para a cadeia completa use o mesmo encadeamento à esquerda (ver §7):

```cpp
const Mat4 mvp = mat4_multiply(projection, mat4_multiply(view, model));
// = P * V * M  → M aplicado primeiro, depois V, depois P.
```

---

## 4. Transformações affine (`mat4_translate`, `mat4_scale`, rotações)

### `mat4_translate(x, y, z)`

```cpp
Mat4 m = mat4_identity();
m[12] = x; m[13] = y; m[14] = z;
```

Matriz (notação linha/coluna):

```
[1  0  0  x]
[0  1  0  y]
[0  0  1  z]
[0  0  0  1]
```

### `mat4_scale(x, y, z)`

```cpp
Mat4 m = mat4_identity();
m[0] = x; m[5] = y; m[10] = z;
```

```
[x  0  0  0]
[0  y  0  0]
[0  0  z  0]
[0  0  0  1]
```

Não há verificação de zero nos fatores: `mat4_scale(0, 0, 0)` é aceito.

### Rotações

As três usam `const float c = std::cos(angle), s = std::sin(angle);` sobre a
identidade. **`angle` está em radianos**, porque `std::cos`/`std::sin` da
biblioteca padrão recebem radianos; o header não faz conversão de graus.

`mat4_rotate_x(angle)` — `m[5]=c; m[6]=s; m[9]=-s; m[10]=c;`

```
[1  0   0  0]
[0  c  -s  0]
[0  s   c  0]
[0  0   0  1]
```

`mat4_rotate_y(angle)` — `m[0]=c; m[2]=-s; m[8]=s; m[10]=c;`

```
[c  0  s  0]
[0  1  0  0]
[-s 0  c  0]
[0  0  0  1]
```

`mat4_rotate_z(angle)` — `m[0]=c; m[1]=s; m[4]=-s; m[5]=c;`

```
[c -s  0  0]
[s  c  0  0]
[0  0  1  0]
[0  0  0  1]
```

Exemplo com radianos:

```cpp
constexpr float pi = 3.14159265f;
const Mat4 meio_giro_z = mat4_rotate_z(pi);       // 180° em Z
const Mat4 quarto_giro_x = mat4_rotate_x(pi / 2.0f); // 90° em X

// Se você tem graus, converta antes (o header não faz isso):
const float rad = 90.0f * pi / 180.0f;
const Mat4 r = mat4_rotate_y(rad);
```

---

## 5. Projeções (`mat4_ortho`, `mat4_ortho2d`, `mat4_perspective`, `mat4_perspective_gl`)

### `mat4_ortho(left, right, bottom, top, near, far)`

```cpp
Mat4 m = mat4_identity();
m[0]  = 2.0f / (right - left);
m[5]  = 2.0f / (top - bottom);
m[10] = -2.0f / (far - near);
m[12] = -(right + left) / (right - left);
m[13] = -(top + bottom) / (top - bottom);
m[14] = -(far + near) / (far - near);
```

| Parâmetro | Significado |
|---|---|
| `left`, `right` | Limites horizontais do volume de visão. |
| `bottom`, `top` | Limites verticais. |
| `near` | Plano próximo (default `-1.0f`). |
| `far` | Plano distante (default `1.0f`). |

A profundidade mapeada é o intervalo `[-1, 1]` (o `-2/(far-near)` e o
`-(far+near)/(far-near)` são a forma OpenGL-style). **Não há verificação de
denominadores zero**: `left == right`, `bottom == top` ou `far == near` geram
divisão por zero (inf/NaN).

```cpp
const Mat4 ortho = mat4_ortho(-2.0f, 2.0f, -1.5f, 1.5f, 0.1f, 100.0f);
const Mat4 ortho_default = mat4_ortho(0.0f, 640.0f, 0.0f, 360.0f);
// ortho_default usa near=-1, far=1.
```

### `mat4_ortho2d(width, height)`

```cpp
return mat4_ortho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
```

Repare na ordem: `bottom = height`, `top = 0`. Isso coloca a origem no **topo**
(eixo Y crescendo para baixo), com X crescendo para a direita. É a projeção que
`GLES2Scene::resize()` instala:

```cpp
projection_ = mat4_ortho2d(static_cast<float>(width_),
                           static_cast<float>(height_));
```

```cpp
const Mat4 proj2d = mat4_ortho2d(800.0f, 600.0f);
// x: 0..800, y: 0..600 (y=0 no topo), z: -1..1
```

### `mat4_perspective(fovy_rad, aspect, near, far)` — Vulkan-style (0..1)

```cpp
Mat4 m{};
const float f = 1.0f / std::tan(fovy_rad / 2.0f);
m[0]  = f / aspect;
m[5]  = f;
m[10] = far / (near - far);
m[11] = -1.0f;
m[14] = (far * near) / (near - far);
```

```
[f/aspect  0    0                 0]
[0         f    0                 0]
[0         0    far/(near-far)    (far*near)/(near-far)]
[0         0   -1                 0]
```

O intervalo de profundidade em NDC é **0..1**, como esperado por Vulkan. O
`fovy_rad` é o **ângulo vertical em radianos**; `aspect` normalmente
`largura / altura`.

### `mat4_perspective_gl(fovy_rad, aspect, near, far)` — OpenGL-style (-1..1)

```cpp
Mat4 m{};
const float f = 1.0f / std::tan(fovy_rad / 2.0f);
m[0]  = f / aspect;
m[5]  = f;
m[10] = (far + near) / (near - far);
m[11] = -1.0f;
m[14] = (2.0f * far * near) / (near - far);
```

```
[f/aspect  0    0                         0]
[0         f    0                         0]
[0         0    (far+near)/(near-far)     (2*far*near)/(near-far)]
[0         0   -1                         0]
```

Mesmos parâmetros e mesmas unidades, mas NDC de profundidade **-1..1**.

Nenhuma das duas funções valida `fovy_rad`, `aspect`, `near` ou `far`; valores
como `aspect == 0`, `near == far`, `fovy_rad == 0` ou `fovy_rad >= pi` podem
gerar divisão por zero, `tan` zero/negativo ou uma projeção sem sentido.

---

## 6. Câmera (`mat4_look_at`)

```cpp
inline Mat4 mat4_look_at(const Vec3& eye, const Vec3& center, const Vec3& up)
```

A implementação segue a construção de base ortonormal a partir da direção de
visão:

1. `z = normalize(eye - center)`. Se o módulo bruto `zl_raw` for `0`, usa
   `zl = 1.0f` (fallback; o vetor z resultante fica zerado).
2. `x = normalize(up × z)`, com produto vetorial escrito diretamente nas
   variáveis `xx, xy, xz`.
3. Se `xl_raw < 1e-6f` — isto é, `up` paralelo à direção de visão (ou `z`
   degenerado) — escolhe um eixo alternativo:

   ```cpp
   const Vec3 alt = (std::fabs(z1) < 0.99f) ? Vec3{0,1,0} : Vec3{0,0,1};
   ```

   O critério usa `z1`, o **componente Y do vetor z**, comparado com `0.99`.
   Depois recalcula `x = normalize(alt × z)`; se ainda assim o módulo for zero,
   `xl = 1.0f` evita divisão por zero.
4. `y = z × x` (não é normalizado novamente).
5. Preenche a matriz como base por colunas e translação negativa:

   ```cpp
   m[0]=x0; m[1]=y0; m[2]=z0;
   m[4]=x1; m[5]=y1; m[6]=z1;
   m[8]=x2; m[9]=y2; m[10]=z2;
   m[12]=-(x0*eye[0] + x1*eye[1] + x2*eye[2]);
   m[13]=-(y0*eye[0] + y1*eye[1] + y2*eye[2]);
   m[14]=-(z0*eye[0] + z1*eye[1] + z2*eye[2]);
   ```

   O `m[15]` vem da identidade inicial.

Como `up` é normalizado indiretamente (pelo módulo do produto vetorial), um
`up` como `{0, 10, 0}` produz a mesma orientação que `{0, 1, 0}`.

```cpp
const Vec3 eye    = {0.0f, 2.0f, 5.0f};
const Vec3 center = {0.0f, 0.0f, 0.0f};
const Vec3 up     = {0.0f, 1.0f, 0.0f};
const Mat4 view = mat4_look_at(eye, center, up);

// up degenerado (paralelo à visão) — cai no fallback:
const Mat4 view2 = mat4_look_at({0,0,5}, {0,0,0}, {0,0,1});
```

**Caso degenerado extremo:** se `eye == center`, `z` fica zero, o fallback de
`x` também gera um produto vetorial zero e a matriz resultante tem base com
zeros — não há NaN por divisão zero, mas a câmera não é válida. Cabe ao chamador
evitar `eye == center`.

---

## 7. Composição: T × R × S e MVP

A ordem canônica de transformação de modelo é **T × R × S**: primeiro escala,
depois rota, depois translada (porque a operação mais à direita age primeiro
sobre o vértice).

```cpp
#include <nativeapp/math.hpp>

using namespace nativeapp;

const Mat4 T = mat4_translate(3.0f, 1.0f, 0.0f);
const Mat4 R = mat4_rotate_z(0.5f);                 // radianos
const Mat4 S = mat4_scale(2.0f, 2.0f, 2.0f);

// model = T * R * S
const Mat4 TR = mat4_multiply(T, R);
const Mat4 model = mat4_multiply(TR, S);

// Equivalente, em uma linha:
const Mat4 model2 = mat4_multiply(
    mat4_multiply(T, R),
    S);
```

Cadeia completa com câmera e projeção:

```cpp
const Vec3 eye    = {0.0f, 0.0f, 5.0f};
const Vec3 center = {0.0f, 0.0f, 0.0f};
const Vec3 up     = {0.0f, 1.0f, 0.0f};

const Mat4 view = mat4_look_at(eye, center, up);
const Mat4 proj = mat4_perspective_gl(
    1.04719755f,        // ~60° em radianos
    800.0f / 600.0f,    // aspect
    0.1f,               // near
    100.0f              // far
);

// P * V * M  (M primeiro, V depois, P por último)
const Mat4 mvp = mat4_multiply(proj, mat4_multiply(view, model));
```

Entregando a matriz ao backend GLES (o `Shader` usa `glUniformMatrix4fv(...,
GL_FALSE, matrix)`):

```cpp
shader.use();
shader.set_uniform_mat4("u_mvp", mvp.data());
```

E ao `Scene::draw()`, que espera `const float* model` com 16 floats
column-major — `nullptr` significa "usar identidade" nas implementações
concretas:

```cpp
scene.draw(mesh, material, model.data());
```

> Atenção: `GLES2Scene` usa sua própria projeção (`mat4_ortho2d` definida em
> `resize()`, alterável com `set_projection()`) e aplica `u_projection *
> u_model * vec4(a_position, 1.0)`. Se você calcular uma MVP completa e
> entregá-la como `model`, a projeção da cena será aplicada **em cima** dela.

---

## 8. Quando usar `perspective` ou `perspective_gl`

| Situação | Função | Faixa de profundidade NDC |
|---|---|---|
| Backend Vulkan (`VulkanScene`) | `mat4_perspective(fovy_rad, aspect, near, far)` | `0..1` |
| Backend OpenGL / GLES (`GLES2Scene`, `GLES3Scene`) | `mat4_perspective_gl(fovy_rad, aspect, near, far)` | `-1..1` |
| Projeção ortográfica 2D da cena GLES | `mat4_ortho2d(width, height)` | `-1..1` (via `mat4_ortho`) |
| Volume ortográfico arbitrário | `mat4_ortho(l, r, b, t, near, far)` | `-1..1` |

Ambas as perspectivas recebem **`fovy_rad` em radianos** e `aspect =
largura / altura`; a diferença está apenas nas linhas `m[10]` e `m[14]`, que
controlam o mapeamento de profundidade.

Os headers não chamam `mat4_perspective` nem `mat4_perspective_gl` internamente
— `GLES2Scene::resize()` usa apenas `mat4_ortho2d()`. A escolha entre uma e
outra é responsabilidade da aplicação, de acordo com o backend que vai consumir
a matriz.

---

## 9. Armadilhas

| Armadilha | Por que acontece | Como evitar |
|---|---|---|
| Ordem de multiplicação invertida | `mat4_multiply(a, b)` é `A*B`; com vetores coluna, `b` age primeiro. `T*R*S ≠ S*R*T`. | Escreva a cadeia como `P * V * M` (proj → view → model) e `T * R * S` (translate → rotate → scale). |
| Achar que índice 12 é "linha 3, coluna 0" | O armazenamento é column-major: índice = `col*4 + row`. 12 = coluna 3, linha 0. | Use o mapa da §1 e pense em colunas. |
| Passar matriz para GLSL transposta | O código já envia column-major com `GL_FALSE`. | Não transponha novamente; use `.data()` direto. |
| Graus em vez de radianos | `std::cos`/`std::sin` e `std::tan` recebem radianos; o header não converte. | Converta: `rad = graus * pi / 180`. |
| `fovy` em radianos confundido com ângulo horizontal | O parâmetro se chama `fovy_rad` (vertical); `aspect` ajusta a horizontal. | Calcule `aspect = largura / altura`. |
| Denominadores zero em `mat4_ortho` | Não há validação de `right-left`, `top-bottom` ou `far-near`. | Garanta limites distintos. |
| `near == far` ou `aspect == 0` na perspectiva | Não há validação; surgem inf/NaN. | Valide os parâmetros antes de chamar. |
| `mat4_ortho2d` com Y "de cabeça para baixo" | A chamada é `mat4_ortho(0, width, height, 0, ...)`: `top = 0`, `bottom = height`. | Trate `y = 0` como topo e `y = height` como base, como faz `GLES2Scene`. |
| `up` paralelo à visão | `up × z` fica zero. | `mat4_look_at` já troca para `{0,1,0}` ou `{0,0,1}` com base em `fabs(z1) < 0.99f`. |
| `eye == center` | `z` fica zero e o fallback não recupera uma base válida. | Nunca use `eye == center`. |
| Esperar `Mat4 * Mat4` ou transformar `Vec3` | O header só tem funções livres sobre arrays; não há operadores nem helper de transformação. | Use `mat4_multiply()` e faça a multiplicação vetor/matriz no shader (como faz o shader padrão de `gl.hpp`). |
| Reutilizar referência a temporário | As funções retornam por valor, sem alias oculto; `Mat4 b = a` copia. | Guarde o resultado em variável (`const Mat4 m = ...`) antes de usar `.data()`. |

---

## 10. Referência rápida

```cpp
#include <nativeapp/math.hpp>

using namespace nativeapp;

// Estruturas
Mat4 identidade = mat4_identity();           // 16 floats column-major
Vec3 posicao = {1.0f, 2.0f, 3.0f};           // 3 floats

// Transformações
Mat4 T = mat4_translate(1.0f, 2.0f, 3.0f);
Mat4 S = mat4_scale(2.0f, 2.0f, 2.0f);
Mat4 RX = mat4_rotate_x(0.5f);               // radianos
Mat4 RY = mat4_rotate_y(0.5f);
Mat4 RZ = mat4_rotate_z(0.5f);

// Composição: T * RZ * S
Mat4 model = mat4_multiply(mat4_multiply(T, RZ), S);

// Projeções
Mat4 ortho  = mat4_ortho(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
Mat4 ortho2 = mat4_ortho2d(800.0f, 600.0f);             // y cresce para baixo
Mat4 pvk    = mat4_perspective(1.0f, 1.333f, 0.1f, 100.0f);    // z 0..1
Mat4 pgl    = mat4_perspective_gl(1.0f, 1.333f, 0.1f, 100.0f); // z -1..1

// Câmera e MVP
Mat4 view = mat4_look_at({0,0,5}, {0,0,0}, {0,1,0});
Mat4 mvp  = mat4_multiply(pgl, mat4_multiply(view, model));

// Uso típico
shader.set_uniform_mat4("u_mvp", mvp.data());
scene.draw(mesh, material, model.data());   // nullptr ⇒ identidade
```

| Precisa de... | Função |
|---|---|
| Matriz neutra | `mat4_identity()` |
| Combinar duas matrizes | `mat4_multiply(a, b)` (`b` primeiro) |
| Mover | `mat4_translate(x, y, z)` |
| Redimensionar | `mat4_scale(x, y, z)` |
| Girar em X/Y/Z | `mat4_rotate_x/y/z(angle)` (radianos) |
| Volume 2D / ortográfico | `mat4_ortho2d(w, h)` / `mat4_ortho(l, r, b, t, near, far)` |
| Perspectiva Vulkan | `mat4_perspective(fovy_rad, aspect, near, far)` |
| Perspectiva OpenGL/GLES | `mat4_perspective_gl(fovy_rad, aspect, near, far)` |
| Posicionar a câmera | `mat4_look_at(eye, center, up)` |
