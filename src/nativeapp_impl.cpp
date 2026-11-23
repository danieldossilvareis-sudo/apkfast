// nativeapp_impl.cpp — implementações third-party (UMA ÚNICA VEZ por app).
//
// O builder do apkfast injeta este arquivo automaticamente quando o projeto
// usa <nativeapp/nativeapp.hpp>. Ter todas as implementações num único TU
// evita duplicação de símbolos em projetos com múltiplos .cpp.

// Configuração do stb_vorbis — DEVE ser idêntica em todos os TUs que veem
// a struct stb_vorbis. Como só este TU inclui a implementação e os headers
// de alto nível incluem com STB_VORBIS_HEADER_ONLY, definimos aqui uma vez.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_MAX_CHANNELS 2
#define STB_VORBIS_NO_COMMENTS

#define STB_IMAGE_IMPLEMENTATION
#include <nativeapp/third_party/stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION

#define STB_TRUETYPE_IMPLEMENTATION
#include <nativeapp/third_party/stb_truetype.h>
#undef STB_TRUETYPE_IMPLEMENTATION

// O stb_vorbis.c é um .c com implementação; incluí-lo diretamente aqui
// (sem HEADER_ONLY) compila o decoder uma única vez.
#include <nativeapp/third_party/stb_vorbis.c>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include <nativeapp/third_party/dr_mp3.h>
#undef DR_MP3_IMPLEMENTATION

// WebP (BitmapFactory/AndroidBitmap) + SVG (NanoSVG).
#include <nativeapp/image_impl.hpp>
