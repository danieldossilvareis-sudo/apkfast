# Fontes TTF/OTF (`font.hpp`)

A v2.0.0 adiciona renderização de texto com **stb_truetype** — sem FreeType,
sem `.so` extra, sem Java.

## Visão geral

`FontAtlas` carrega um TTF/OTF de `assets/`, gera um atlas de glyphs ASCII
(32..126) e expõe quads com UVs. `GLES2Scene` usa o atlas para desenhar texto
com `draw_text()`. O `build()` é **transacional** (falha não deixa atlas
"válido" pela metade), `quad()` tem guards e `valid()` consulta uma flag
explícita.

```cpp
#include <nativeapp/nativeapp.hpp>

void on_surface_created() override {
    scene_.init(window());
    scene_.load_font("font.ttf", 28);  // caminho RELATIVO a assets/
}

void on_draw() override {
    scene_.draw_text("Olá, apkfast!", 40, 120, 1.5f,
                     0.37f, 0.79f, 0.44f, 1.0f);  // r,g,b,a
}
```

## API `FontAtlas`

| Método | Descrição |
|---|---|
| `load(path, pixel_size=24, atlas_size=kDefaultSize)` | Carrega TTF de assets e chama `build()` |
| `build(ttf, size, pixel_size=24, atlas_size=kDefaultSize)` | Constrói o atlas a partir de memória; **transacional** |
| `quad(c, x, y, scale)` | Retorna `GlyphQuad` e avança `x`; **protegido** contra atlas inválido/caracteres fora do ASCII |
| `text_width(text, scale)` | Largura aproximada |
| `atlas()` / `atlas_size()` | Dados brutos para upload GL |
| `valid()` | `valid_`, flag explícita (só `true` após `build()` bem-sucedido) |

Constantes públicas: `kDefaultSize = 512`, `kFirstChar = 32`, `kCharCount = 95`.
Cópia é proibida (`FontAtlas(const FontAtlas&) = delete` e
`operator=(...) = delete`); **move é explícito** (`FontAtlas(FontAtlas&&)
noexcept = default` e `FontAtlas& operator=(FontAtlas&&) noexcept = default`).

## API `GLES2Scene` (extensões v2)

| Método | Descrição |
|---|---|
| `load_font(path, pixel_size=24)` | Carrega e faz upload do atlas |
| `draw_text(text, x, y, scale, r,g,b,a)` | Desenha com coordenadas de tela |
| `has_font()` | Fonte carregada |

## Detalhes de robustez

### `build()` é transacional

```cpp
bool build(const uint8_t* ttf, int ttf_size, int pixel_size = 24,
           int atlas_size = kDefaultSize) {
    reset();
    if (!ttf || ttf_size <= 0 || pixel_size <= 0 || atlas_size <= 0) return false;
    // ... stbtt_PackBegin / PackSetOversampling(2,2) / PackFontRange ...
    if (!stbtt_PackBegin(...)) { reset(); return false; }
    const int ok = stbtt_PackFontRange(...);
    stbtt_PackEnd(&pack);
    if (ok == 0) { reset(); return false; }
    valid_ = true;
    return true;
}
```

Entradas inválidas, falha no `PackBegin` ou falha no `PackFontRange` chamam
`reset()` (limpa `atlas_`, `chars_`, `atlas_size_` e `valid_`) antes de devolver
`false`. Ou seja: **depois de uma falha, `valid() == false` e não há atlas
parcial** que pareça pronto. `load()` repassa direto para `build()`, com o mesmo
comportamento. O código usa `ttf` como `const` — não há `const_cast`.

### `valid()` e `quad()` protegidos

```cpp
bool valid() const { return valid_; }

GlyphQuad quad(char c, float& x, float y, float scale = 1.0f) const {
    stbtt_aligned_quad q{};
    const int index = static_cast<int>(static_cast<uint8_t>(c)) - kFirstChar;
    if (!valid_ || index < 0 || index >= kCharCount ||
        static_cast<std::size_t>(index) >= chars_.size() || atlas_size_ <= 0) {
        return GlyphQuad{x, y, x, y, 0, 0, 0, 0};  // quad vazio, sem avançar x
    }
    stbtt_GetPackedQuad(chars_.data(), atlas_size_, atlas_size_, index,
                        &x, &y, &q, 1);
    return GlyphQuad{q.x0 * scale, q.y0 * scale, q.x1 * scale, q.y1 * scale,
                     q.s0, q.t0, q.s1, q.t1};
}
```

O guarda cobre atlas inválido (build falhou), caractere fora de 32..126 e
dessincronia entre `index` e `chars_.size()`. Sem ele, `chars_.data()` poderia
ser nulo e o stbtt leria fora do array. No caminho protegido, o quad de retorno
é degenerado e **`x` não avança**. O `1` final de `stbtt_GetPackedQuad` indica
coordenadas OpenGL (Y para cima); `scale` multiplica posições, não UVs.

## Armadilhas

- **Caminhos de assets são relativos**: use `"font.ttf"`, não `"assets/font.ttf"`
  (o `-A` do aapt2 já coloca o conteúdo em `assets/`).
- Apenas ASCII imprimível (32..126) no atlas padrão; fora disso, `quad()`
  devolve um quad vazio em vez de estourar o array de glyphs.
- Se `build()` falhar, chame `valid()` antes de usar a fonte: o estado é limpo
  e não existe atlas parcial. Verifique também o retorno de `load()`.
- `FontAtlas` não pode ser copiada; use move explícito (`std::move`) se precisar
  transferi-la.
- Use fontes confiáveis — stb_truetype não tem garantia de segurança para
  arquivos maliciosos.
- Kerning/RTL/ligaduras completas exigem FreeType (fora de escopo).
- `draw_text()` usa a projection atual da cena (coordenadas de tela após
  `resize()`).
