// nativeapp/image_codecs.hpp — decode de imagens: raster, WebP e SVG.
//
// PNG/JPEG/BMP/... continuam com stb_image. WebP é decodificado pelo Android
// (BitmapFactory + AndroidBitmap_*, sem Java customizado) e SVG pelo NanoSVG
// rasterizado para RGBA. As implementações WebP/SVG vivem no TU injetado pelo
// builder (src/nativeapp_impl*.cpp), uma única vez por app.

#pragma once

#include "assets.hpp"
#include "third_party/stb_image.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nativeapp {

enum class ImageFormat {
    Unknown,
    Raster,  // PNG/JPEG/GIF/BMP/... (stb_image)
    Webp,
    Svg,
};

struct DecodedImage {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

/// Detecta o formato pelos magic bytes (extensão não é confiável em assets).
inline ImageFormat detect_image_format(const uint8_t* data, size_t size) {
    if (!data || size < 4) return ImageFormat::Unknown;

    if (size >= 12 && std::memcmp(data, "RIFF", 4) == 0 &&
        std::memcmp(data + 8, "WEBP", 4) == 0) {
        return ImageFormat::Webp;
    }
    if (size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
        data[3] == 'G' && data[4] == 0x0d && data[5] == 0x0a &&
        data[6] == 0x1a && data[7] == 0x0a) {
        return ImageFormat::Raster;
    }
    if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff) {
        return ImageFormat::Raster;
    }

    // SVG é texto: ignora BOM/whitespace e procura <svg (ou <?xml ... <svg).
    size_t index = 0;
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
        index = 3;
    }
    while (index < size && std::isspace(static_cast<unsigned char>(data[index]))) {
        ++index;
    }
    if (size - index >= 4 && std::memcmp(data + index, "<svg", 4) == 0) {
        return ImageFormat::Svg;
    }
    if (size - index >= 5 && std::memcmp(data + index, "<?xml", 5) == 0) {
        const size_t limit = index + (size - index > 1024 ? 1024 : size - index);
        for (size_t cursor = index; cursor + 4 <= limit; ++cursor) {
            if (std::memcmp(data + cursor, "<svg", 4) == 0) {
                return ImageFormat::Svg;
            }
        }
    }
    // Outros formatos suportados pelo stb_image (GIF/BMP/TGA/...).
    return ImageFormat::Raster;
}

// Definidos em src/nativeapp_impl.cpp e nativeapp_impl_no_image.cpp.
bool decode_webp_jni(const uint8_t* data, size_t size, DecodedImage& out);
bool decode_webp_full(const uint8_t* data, size_t size, DecodedImage& out);
bool decode_svg_nano(const uint8_t* data, size_t size, DecodedImage& out,
                     int width, int height);
bool decode_svg_full(const uint8_t* data, size_t size, DecodedImage& out,
                     int width, int height);
bool decode_svg_auto(const uint8_t* data, size_t size, DecodedImage& out,
                     int width, int height);

/// Registra uma fonte TTF/OTF de assets/ para texto em SVG (LunaSVG).
bool svg_register_font_asset(const char* family, const std::string& asset,
                             bool bold = false, bool italic = false);

/// Decodifica bytes em RGBA. `svg_w`/`svg_h` controlam a rasterização SVG:
/// se ambos forem 0, usa o tamanho natural do documento.
inline bool decode_image_bytes(const uint8_t* data, size_t size,
                               DecodedImage& out,
                               int svg_w = 0, int svg_h = 0) {
    if (!data || size == 0) return false;
    const ImageFormat format = detect_image_format(data, size);
    if (format == ImageFormat::Unknown) return false;
    if (format == ImageFormat::Webp) {
#ifdef APKFAST_WEBP_FULL
        return decode_webp_full(data, size, out);
#else
        return decode_webp_jni(data, size, out);
#endif
    }
    if (format == ImageFormat::Svg) {
        return decode_svg_auto(data, size, out, svg_w, svg_h);
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(
        data, static_cast<int>(size), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgba.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

/// Lê um asset e decodifica. Requer Assets::instance(activity).set_activity().
inline bool decode_asset_image(const std::string& path, DecodedImage& out,
                               int svg_w = 0, int svg_h = 0) {
    auto& assets = Assets::instance(nullptr);
    if (!assets.manager()) return false;
    const auto data = assets.read(path);
    if (data.empty()) return false;
    return decode_image_bytes(data.data(), data.size(), out, svg_w, svg_h);
}

}  // namespace nativeapp
