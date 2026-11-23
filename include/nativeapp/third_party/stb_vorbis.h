// stb_vorbis.h — wrapper header-only para stb_vorbis.c.
//
// As macros de configuração DEVEM ser iguais às usadas em nativeapp_impl.cpp.
#pragma once

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_MAX_CHANNELS 2
#define STB_VORBIS_NO_COMMENTS
#include "stb_vorbis.c"
#undef STB_VORBIS_NO_COMMENTS
#undef STB_VORBIS_MAX_CHANNELS
#undef STB_VORBIS_NO_STDIO
#undef STB_VORBIS_HEADER_ONLY
