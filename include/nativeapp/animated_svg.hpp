// nativeapp/animated_svg.hpp — SVG animado declarativo (subconjunto SMIL).
//
// Requer LunaSVG (`python3 apkfast.py lunasvg-install`) quando o documento
// contém <animate>, <animateTransform> ou <set>. SVGs sem SMIL continuam no
// caminho estático decode_svg_auto() (NanoSVG para ícones simples).
// As implementações vivem em image_impl.hpp, injetado uma única vez pelo
// builder; este header é seguro de incluir em qualquer TU da app.

#pragma once

#include "image_codecs.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace nativeapp {

class AnimatedSvg {
public:
    AnimatedSvg();
    ~AnimatedSvg();
    AnimatedSvg(const AnimatedSvg&) = delete;
    AnimatedSvg& operator=(const AnimatedSvg&) = delete;

    /// SMIL sem LunaSVG retorna false (com aviso); SVG estático funciona.
    bool open(const uint8_t* data, size_t size);
    bool open_asset(const std::string& path);

    bool is_open() const { return impl_ != nullptr; }
    void close();

    int width() const;
    int height() const;
    /// Fim do último intervalo finito; 0 = estático ou loop infinito.
    float duration_ms() const;
    /// 0 = infinito, 1 = uma vez (padrão para documentos estáticos).
    int loop_count() const;
    size_t animation_count() const;

    /// Avança o relógio (ms) e re-renderiza se algum atributo mudou.
    bool update(float dt_ms);
    void reset();

    /// RGBA não-premultiplicado. Válido enquanto o objeto estiver aberto.
    const DecodedImage& current() const;

    /// Troca a rasterização (0,0 = tamanho natural). Chamada antes ou depois de open().
    bool set_size(int width, int height);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    DecodedImage empty_;
    int svg_w_ = 0;
    int svg_h_ = 0;
};

}  // namespace nativeapp
