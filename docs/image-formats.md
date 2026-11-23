# Imagens: PNG/JPEG, WebP, WebP animado e SVG — v2.3.0

A `Texture` detecta o formato pelos **magic bytes**, não pela extensão.
O mesmo `Material::texture = "icone.svg"` funciona em `GLES2Scene`,
`GLES3Scene` e em `Texture` direto.

## Formatos suportados

| Formato | Decoder | Observações |
|---|---|---|
| PNG/JPEG/GIF/BMP/TGA/HDR/PSD/PNM | stb_image | caminho original |
| WebP estático | libwebp 1.6 (ou BitmapFactory se lib não instalada) | alpha correto |
| WebP animado | libwebp `WebPAnimDecoder` | todos os frames, duração e loop |
| SVG | LunaSVG 3.5.0+filters | CSS, texto, clipPath, mask, pattern, filtros |
| SVG simples | NanoSVG | fallback leve/compatível |

Instale os decodificadores avançados uma vez:

```sh
python3 apkfast.py webp-install --abi arm64-v8a
python3 apkfast.py lunasvg-install
# ou todas as libs/ABIs:
python3 apkfast.py setup-libs --abis all
```

## API estática

```cpp
#include <nativeapp/nativeapp.hpp>

nativeapp::Texture tex;
tex.load("sprite.png");                     // detecta sozinho
tex.load_webp_asset("sprite.webp");         // alias explícito
tex.load_svg_asset("logo.svg", 256, 256);   // rasteriza em 256x256
tex.load_svg_asset("logo.svg");             // tamanho natural
tex.load_rgba(pixels, width, height);       // RGBA pronto
```

Decode sem criar textura:

```cpp
nativeapp::DecodedImage image;
nativeapp::decode_asset_image("logo.svg", image, 128, 128);
```

## WebP animado

```cpp
nativeapp::AnimatedImage anim;
if (anim.open_asset("animated.webp")) {
    LOGI("frames=%zu loop=%d", anim.frame_count(), anim.loop_count());
}

nativeapp::Texture frame;
const auto& first = anim.current_frame();
frame.load_rgba(first.rgba.data(), first.width, first.height);

// a cada frame do app (dt em segundos):
if (anim.update(dt * 1000.0f)) {
    const auto& current = anim.current_frame();
    frame.load_rgba(current.rgba.data(), current.width, current.height);
}
scene.draw_with_texture(mesh, material, nullptr, &frame);
```

`AnimatedImage` expõe:

- `frame_count()`, `loop_count()` (`0` = infinito)
- `frame(i)`, `current_frame()`, `current_index()`
- `update(dt_ms)` — avança o relógio e troca de frame
- `reset()`, `set_looping(true)`, `looping()`
- `open(bytes, size)`, `open_asset(path)`

O decoder usa `WebPAnimDecoder`, que já aplica blend/dispose e devolve o canvas
composto em RGBA não-premultiplicado. Limite de segurança: 128 MiB por asset.

## SVG completo (LunaSVG)

O LunaSVG cobre:

- `<style>`, atributo `style`, seletores/classes/especificidade/`!important`
- `<text>`, `<tspan>`, fontes TTF/OTF carregadas de `assets/`
- `clipPath`, `mask`, `pattern`, gradientes, `transform`, `use`, `marker`
- filtros SVG 1.1 (patch incluído), como `feGaussianBlur`/`feDropShadow`

Registre fontes antes de carregar SVGs com texto:

```cpp
nativeapp::svg_register_font_asset("DejaVu Sans", "font.ttf");
nativeapp::svg_register_font_asset("sans-serif", "font.ttf");
nativeapp::svg_register_font_asset("Arial", "font.ttf", true, false);
```

A API `decode_svg_auto()` usa LunaSVG quando o documento contém marcadores
avançados e mantém NanoSVG para ícones simples. Se LunaSVG falhar, há fallback
automático para NanoSVG.

## raylib

Nenhum símbolo `stbi_*` é redefinido. O builder injeta
`nativeapp_impl_no_image.cpp` quando raylib está presente, mantendo WebP/SVG.
As TUs do LunaSVG são independentes.

## Correção de UV

`MeshData::quad()` agora usa V invertido. stb, libwebp e LunaSVG entregam a
primeira linha no topo, enquanto `GL_TEXTURE_2D` coloca `t=0` na base. Antes
desta versão, texturas em quads apareciam verticalmente invertidas.

## Exemplo

```sh
python3 apkfast.py build examples/image-demo -o image-demo.apk
python3 apkfast.py install image-demo.apk
python3 apkfast.py logs -s image-demo
```

Saída esperada:

```text
I image-demo: webp=OK 64x64 svg=OK 256x256 font=OK anim=OK frames=3 loop=0
I image-demo: anim frame=2/3
```
