# Documentação da API nativeapp

Guias completos e independentes para cada helper do apkfast. Cada guia é
baseado no código real dos headers em `include/nativeapp/` — não são resumos:
trazem tabelas de API, exemplos compiláveis, integração com o `App` e
armadilhas encontradas em testes reais.

## Núcleo

| Guia | O que cobre |
|---|---|
| [app.md](app.md) | Ciclo de vida, looper, `on_update(dt)`, `set_scene()`, input, `APP_MAIN` |
| [graphics-types.md](graphics-types.md) | `Vertex`, `MeshData`, `Material`, `Topology`, interface `Scene` |
| [assets.md](assets.md) | Leitura de arquivos em `assets/` via `AAssetManager` |
| [math.md](math.md) | `Mat4`, projeções, rotações, `look_at`, ordem de multiplicação |

## Gráficos

| Guia | O que cobre |
|---|---|
| [graphics-opengl.md](graphics-opengl.md) | `GLContext`, `Shader`, `Mesh`, `Texture`, `GLES2Scene`, `GLES3Scene` |
| [graphics-vulkan.md](graphics-vulkan.md) | `VulkanScene`, `GraphicsPipeline`, `GpuBuffer`, depth, frames in-flight |

## Interação e sistema

| Guia | O que cobre |
|---|---|
| [text-input.md](text-input.md) | Teclado virtual sem EditText, IME/unicode, API 21+ |
| [fonts.md](fonts.md) | Fontes TTF/OTF, atlas de glyphs, `draw_text()` |
| [audio-codecs.md](audio-codecs.md) | WAV/OGG/MP3, decode completo e streaming `Music` |
| [image-formats.md](image-formats.md) | PNG/JPEG, WebP animado (libwebp) e SVG completo (LunaSVG) |
| [animated-svg.md](animated-svg.md) | SMIL declarativo (`animate`/`animateTransform`/`set`) via LunaSVG |
| [libraries.md](libraries.md) | Bibliotecas `.a`/`.so` próprias por ABI |
| [incremental.md](incremental.md) | Cache incremental portável (schema 2) |
| [cache-remote.md](cache-remote.md) | Cache compartilhado, bundles e HMAC |
| [cache-transport.md](cache-transport.md) | `cache-push`/`cache-pull` por file/HTTP/S3 sobre o mesmo `.tar.gz` |
| [permissions.md](permissions.md) | Permissões runtime, callbacks, estados, todas as engines |
| [input-helpers.md](input-helpers.md) | Mouse, gamepad analógico, estado de teclas |
| [sensors.md](sensors.md) | Acelerômetro/giroscópio via callbacks no looper |
| [device.md](device.md) | Vibração, bateria, orientação, modelo, brilho |
| [audio.md](audio.md) | AAudio + OpenSLES, WAV de assets, limitações atuais |

## Como usar estes guias

1. Comece por [app.md](app.md) — quase tudo parte do `App`.
2. Para desenhar, leia [graphics-types.md](graphics-types.md) e depois
   [graphics-opengl.md](graphics-opengl.md) ou [graphics-vulkan.md](graphics-vulkan.md).
3. Para interação, [text-input.md](text-input.md),
   [permissions.md](permissions.md) e [input-helpers.md](input-helpers.md).
4. Para elementos do sistema, [sensors.md](sensors.md), [device.md](device.md)
   e [audio.md](audio.md).

Todos os exemplos citados estão em `examples/` na raiz do repositório. Para
compilar qualquer um:

```sh
python3 apkfast.py build examples/<nome> -o app.apk
python3 apkfast.py install app.apk
```

## Documentação relacionada

- [README principal](../README.md) — instalação, CLI, setup-sdk/setup-libs
- [README da API](../include/nativeapp/README.md) — referência consolidada
- [AUDIT.md](../AUDIT.md) — auditoria completa e bugs corrigidos
- [Compatibilidade](compatibility.md) — SemVer da API nativeapp
- [Migração](migration.md) — 2.2 → 2.3 → 2.5
- [AAB / Google Play](aab.md) — bundletool, assinatura e publicação
- [Assinatura](signing.md) — v1/v2/v3/v4, keystore e versionCode
- [Cache de build](build-cache.md) — recursos, APK assinado e diagnóstico
- [Publicação](publishing.md) — PyPI, TestPyPI e release
- [CI em dispositivo](device-ci.md) — self-hosted e Firebase Test Lab
- [CHANGELOG.md](../CHANGELOG.md) — histórico de versões
