# Fontes TTF/OTF (`font.hpp`)

A v2.0.0 adiciona renderização de texto com **stb_truetype** — sem FreeType,
sem `.so` extra, sem Java.

## Visão geral

`FontAtlas` carrega um TTF/OTF de `assets/`, gera um atlas de glyphs ASCII
(32..126) e expõe quads com UVs. `GLES2Scene` usa o atlas para desenhar texto
com `draw_text()`.

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
| `load(path, pixel_size=24, atlas_size=512)` | Carrega TTF de assets |
| `build(ttf_bytes, size, pixel_size, atlas_size)` | Construction a partir de memória |
| `quad(c, x, y, scale)` | Retorna `GlyphQuad` e avança `x` |
| `text_width(text, scale)` | Largura aproximada |
| `atlas()` / `atlas_size()` | Dados brutos para upload GL |
| `valid()` | Atlas pronto |

## API `GLES2Scene` (extensões v2)

| Método | Descrição |
|---|---|
| `load_font(path, pixel_size=24)` | Carrega e faz upload do atlas |
| `draw_text(text, x, y, scale, r,g,b,a)` | Desenha com coordenadas de tela |
| `has_font()` | Fonte carregada |

## Armadilhas

- **Caminhos de assets são relativos**: use `"font.ttf"`, não `"assets/font.ttf"`
  (o `-A` do aapt2 já coloca o conteúdo em `assets/`).
- Apenas ASCII imprimível (32..126) no atlas padrão.
- Use fontes confiáveis — stb_truetype não tem garantia de segurança para
  arquivos maliciosos.
- Kerning/RTL/ligaduras completas exigem FreeType (fora de escopo).
- `draw_text()` usa a projection atual da cena (coordenadas de tela após
  `resize()`).
