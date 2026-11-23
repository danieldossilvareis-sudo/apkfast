// nativeapp/animated_image.hpp — WebP animado com frames/timing/loop.
//
// Requer libwebp instalada (`python3 apkfast.py webp-install`). Sem ela,
// decode_webp() continua funcionando apenas para o primeiro frame via JNI.

#pragma once

#include "assets.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct WebPData;
struct WebPAnimDecoder;

namespace nativeapp {

class AnimatedImage {
public:
    struct Frame {
        std::vector<uint8_t> rgba;  // canvas completo, RGBA não-premultiplicado
        int width = 0;
        int height = 0;
        int duration_ms = 0;
    };

    AnimatedImage();
    ~AnimatedImage();
    AnimatedImage(const AnimatedImage&) = delete;
    AnimatedImage& operator=(const AnimatedImage&) = delete;

    /// Aceita WebP estático (1 frame) ou animado. Decodifica todos os frames.
    bool open(const uint8_t* data, size_t size);
    bool open_asset(const std::string& path);

    bool is_open() const { return !frames_.empty(); }
    void close();

    int width() const { return width_; }
    int height() const { return height_; }
    size_t frame_count() const { return frames_.size(); }
    int loop_count() const { return loop_count_; }  // 0 = infinito

    const Frame& frame(size_t index) const;
    const Frame& current_frame() const;
    size_t current_index() const { return index_; }

    /// Avança o relógio em milissegundos; true se o frame visível mudou.
    bool update(float dt_ms);

    void reset();
    void set_looping(bool enabled) { force_loop_ = enabled; }
    bool looping() const { return force_loop_; }

private:
    std::vector<uint8_t> bytes_;
    WebPData* data_ = nullptr;
    WebPAnimDecoder* decoder_ = nullptr;
    std::vector<Frame> frames_;
    size_t index_ = 0;
    float elapsed_ms_ = 0.0f;
    uint32_t plays_ = 0;
    bool force_loop_ = false;
    int width_ = 0;
    int height_ = 0;
    int loop_count_ = 1;
};

}  // namespace nativeapp
