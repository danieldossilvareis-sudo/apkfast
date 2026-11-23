// nativeapp/font.hpp — Fontes TTF/OTF via stb_truetype.
//
// As implementações vivem em src/nativeapp_impl.cpp (injetado pelo builder).
// FontAtlas gera um atlas de glyphs e fornece quads para renderização.

#pragma once

#include "assets.hpp"

#include <nativeapp/third_party/stb_truetype.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nativeapp {

struct GlyphQuad {
    float x0, y0, x1, y1;   // posição
    float u0, v0, u1, v1;   // UV no atlas
};

class FontAtlas {
public:
    static constexpr int kDefaultSize = 512;
    static constexpr int kFirstChar = 32;
    static constexpr int kCharCount = 95;  // ASCII imprimível

    FontAtlas() = default;
    ~FontAtlas() = default;

    /// Carrega TTF/OTF de assets/ e monta o atlas com `pixel_size`.
    bool load(const std::string& path, int pixel_size = 24, int atlas_size = kDefaultSize) {
        auto& assets = Assets::instance(nullptr);
        if (!assets.manager()) return false;
        const auto bytes = assets.read(path);
        if (bytes.empty()) return false;
        return build(bytes.data(), static_cast<int>(bytes.size()), pixel_size, atlas_size);
    }

    /// Monta o atlas a partir de bytes já em memória.
    bool build(const uint8_t* ttf, int ttf_size, int pixel_size = 24,
               int atlas_size = kDefaultSize) {
        if (!ttf || ttf_size <= 0) return false;
        atlas_size_ = atlas_size;
        atlas_.assign(static_cast<size_t>(atlas_size_) * atlas_size_, 0);
        chars_.assign(kCharCount, stbtt_packedchar{});

        stbtt_pack_context pack{};
        if (!stbtt_PackBegin(&pack, atlas_.data(), atlas_size_, atlas_size_,
                            0, 1, nullptr)) return false;
        stbtt_PackSetOversampling(&pack, 2, 2);
        const int ok = stbtt_PackFontRange(&pack, const_cast<uint8_t*>(ttf), 0,
                                           static_cast<float>(pixel_size),
                                           kFirstChar, kCharCount, chars_.data());
        stbtt_PackEnd(&pack);
        return ok != 0;
    }

    /// Retorna o quad de um caractere ASCII, avançando `x`.
    GlyphQuad quad(char c, float& x, float y, float scale = 1.0f) const {
        stbtt_aligned_quad q{};
        const int index = static_cast<int>(static_cast<uint8_t>(c)) - kFirstChar;
        if (index < 0 || index >= kCharCount) {
            return GlyphQuad{x, y, x, y, 0, 0, 0, 0};
        }
        stbtt_GetPackedQuad(chars_.data(), atlas_size_, atlas_size_, index,
                            &x, &y, &q, 1);  // 1 = coordenadas OpenGL (y para cima)
        return GlyphQuad{
            q.x0 * scale, q.y0 * scale, q.x1 * scale, q.y1 * scale,
            q.s0, q.t0, q.s1, q.t1,
        };
    }

    /// Largura aproximada de um texto no tamanho do atlas.
    float text_width(const std::string& text, float scale = 1.0f) const {
        float x = 0, y = 0;
        for (char c : text) { quad(c, x, y, scale); }
        return x;
    }

    const uint8_t* atlas() const { return atlas_.data(); }
    int atlas_size() const { return atlas_size_; }
    bool valid() const { return !atlas_.empty(); }

private:
    std::vector<uint8_t> atlas_;
    std::vector<stbtt_packedchar> chars_;
    int atlas_size_ = 0;
};

}  // namespace nativeapp
