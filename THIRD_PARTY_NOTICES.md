# Avisos de terceiros

O apkfast inclui ou prepara sob demanda os seguintes componentes. Os arquivos
de licença originais são preservados junto ao código quando disponíveis.

| Componente | Licença | Onde |
|---|---|---|
| stb_image, stb_truetype, stb_vorbis | domínio público / MIT (stb) | `include/nativeapp/third_party/stb_image.h`, `stb_truetype.h`, `stb_vorbis.h`, `stb_vorbis.c`; TU de inclusão em `src/nativeapp_impl.cpp` |
| dr_mp3 | domínio público (dr_libs) | `include/nativeapp/third_party/dr_mp3.h`; TU em `src/nativeapp_impl.cpp` |
| NanoSVG / NanoSVGRaster | zlib | `include/nativeapp/third_party/nanosvg.h`, `nanosvgrast.h`, `NANOSVG-LICENSE` |
| LunaSVG | MIT | fonte baixada em `vendor/lunasvg/src` por `apkfast lunasvg-install`; patch em `patches/lunasvg-filters.diff`; versão fixa em `apkfast/lunasvg.py` |
| PlutoVG (parte do LunaSVG) | MIT | `vendor/lunasvg/src/plutovg/` (mesmo tarball do LunaSVG) |
| libwebp (decoder + demux) | BSD-3-Clause + concessão de patentes ("PATENTS") | fonte em `vendor/webp/src`, headers/licenças copiados para `vendor/webp/include/webp/` por `apkfast webp-install`; versão/checksum em `apkfast/webp.py` |
| raylib, SDL2, SDL3 | zlib / zlib / zlib (conforme upstream) | baixados por `apkfast setup-libs` para `vendor/{raylib,sdl2,sdl3}`; não redistribuídos na wheel |

## Notas

- `vendor/` é ignorado pelo Git e não é versionado: `setup-libs` baixa fontes
  com SHA256 verificado e compila para a ABI solicitada.
- `patches/lunasvg-filters.diff` é mantido pelo apkfast e é aplicado sobre o
  commit fixo registrado em `apkfast/lunasvg.py`.
- Quando o apkfast é instalado via pip, `include/`, `src/`, `patches/` e
  `docs/` são instalados junto com o pacote Python (ver `pyproject.toml`),
  mantendo os avisos de licença acessíveis.
- Este projeto é licenciado sob MIT (`LICENSE`); este documento não substitui
  as licenças dos componentes listados acima.
