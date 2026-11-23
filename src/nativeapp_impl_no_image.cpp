// nativeapp_impl_no_image.cpp — TU third-party sem stb_image.
// Usado quando raylib está presente (ela já fornece os símbolos stbi_*).

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_MAX_CHANNELS 2
#define STB_VORBIS_NO_COMMENTS

#define STB_TRUETYPE_IMPLEMENTATION
#include <nativeapp/third_party/stb_truetype.h>
#undef STB_TRUETYPE_IMPLEMENTATION

#include <nativeapp/third_party/stb_vorbis.c>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include <nativeapp/third_party/dr_mp3.h>
#undef DR_MP3_IMPLEMENTATION
