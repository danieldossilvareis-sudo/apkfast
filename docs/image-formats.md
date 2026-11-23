# Imagens: PNG/JPEG, WebP e SVG — v2.2.0

A `Texture` do nativeapp detecta o formato pelos **magic bytes**, não pela
extensão. O mesmo `Material::texture = "icone.svg"` funciona em `GLES2Scene`,
`GLES3Scene` e em `Texture` direto.

## Formatos suportados

| Formato | Decoder | Observações |
|---|---|---|
| PNG/JPEG/GIF/BMP/TGA/HDR/PSD/PNM | stb_image | caminho original, inalterado |
| WebP (lossy/lossless/alpha) | `BitmapFactory` + `AndroidBitmap_*` via JNI | primeiro frame de WebP animado |
| SVG | NanoSVG → RGBA | subconjunto útil para ícones |

## API

```cpp
#include <nativeapp/nativeapp.hpp>

nativeapp::Texture tex;

tex.load("sprite.png");                    // detecta sozinho
tex.load_webp_asset("sprite.webp");        // alias explícito
tex.load_svg_asset("logo.svg", 256, 256);  // rasteriza em 256x256
tex.load_svg_asset("logo.svg");            // usa width/height do SVG

tex.width();
tex.height();
tex.bind(0);
```

Também é possível decodificar sem criar uma textura:

```cpp
nativeapp::DecodedImage image;
if (nativeapp::decode_asset_image("logo.svg", image, 128, 128)) {
    // image.rgba, image.width, image.height
}
```

`decode_image_bytes()` faz o mesmo a partir de memória.

## Como o WebP funciona sem Java customizado

O `App` injeta o `ANativeActivity` no singleton `JniBitmap` durante
`on_ready()`, da mesma forma que já faz com `Assets` e `TextInput`. O decoder:

1. lê o asset com `AAssetManager`
2. chama `BitmapFactory.decodeByteArray()` com `inPreferredConfig=ARGB_8888`,
   `inScaled=false` e `inPremultiplied=false`
3. copia os pixels com `AndroidBitmap_lockPixels()`, respeitando `info.stride`
4. desmultiplica o alpha se o device ignorar `inPremultiplied=false`

O builder linka `-ljnigraphics` automaticamente quando a API nativeapp está
ativa. Em runtime, o Android fornece a biblioteca real.

## SVG (NanoSVG)

Os headers `third_party/nanosvg.h` e `nanosvgrast.h` são vendorizados com
licença zlib (`NANOSVG-LICENSE`). As implementações ficam no TU injetado
`src/nativeapp_impl*.cpp`, uma única vez por app.

Cobertura: paths, formas básicas, transforms, fill/stroke e gradientes
lineares/radiais simples. **Não** cobre CSS, texto, `clipPath`, máscaras,
filtros ou patterns. Para SVG 1.1 completo, compile uma lib maior e use
`--lib` (ver [libraries.md](libraries.md)).

A rasterização preserva a proporção e centraliza o resultado quando a razão
de `width/height` pedida difere da do documento. Limite de segurança:
4096×4096 pixels.

## raylib

Nenhum símbolo `stbi_*` é redefinido. Quando raylib está presente, o builder
injeta `nativeapp_impl_no_image.cpp`, que não define
`STB_IMAGE_IMPLEMENTATION`, mas continua fornecendo WebP/SVG.

## Exemplo completo

```sh
python3 apkfast.py build examples/image-demo -o image-demo.apk
python3 apkfast.py install image-demo.apk
python3 apkfast.py logs -s image-demo
```

Saída esperada no dispositivo:

```text
I image-demo: webp=OK 64x64 svg=OK 256x256
```

## Limitações conhecidas

- WebP animado: apenas o primeiro frame (animação exigiria `ImageDecoder`,
  API 28+, e Java customizado).
- WebP precisa de JNI inicializado — fora de um `App`, chame
  `JniBitmap::instance(activity)` manualmente.
- SVG com fonte externa não é renderizado (NanoSVG não faz texto).
