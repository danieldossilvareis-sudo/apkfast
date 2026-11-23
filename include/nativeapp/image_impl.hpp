// nativeapp/image_impl.hpp — implementações WebP/SVG (somente o TU injetado).
//
// Este header NÃO deve ser incluído por código de usuário: ele define símbolos
// e liga as implementações NanoSVG. É incluído apenas por
// src/nativeapp_impl.cpp / nativeapp_impl_no_image.cpp.

#pragma once

#include "animated_image.hpp"
#include "image_codecs.hpp"
#include "jni_bitmap.hpp"

#ifdef APKFAST_WEBP_FULL
#include <webp/demux.h>
#endif

#ifdef APKFAST_SVG_FULL
#include <lunasvg.h>
#endif

#include <android/bitmap.h>
#include <android/log.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// NanoSVG (zlib). Uma única TU por app define as implementações.
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg.h"
#undef NANOSVG_IMPLEMENTATION

#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvgrast.h"
#undef NANOSVGRAST_IMPLEMENTATION

namespace nativeapp {
namespace image_detail {

inline void warn(const char* message) {
    __android_log_print(ANDROID_LOG_WARN, "nativeapp-image", "%s", message);
}

}  // namespace image_detail

bool decode_webp_jni(const uint8_t* data, size_t size, DecodedImage& out) {
    JniBitmap& holder = JniBitmap::instance();
    JNIEnv* env = holder.env();
    const jobject activity = holder.activity();
    if (!env || !activity) {
        image_detail::warn("WebP: JNI não inicializado (App não injetou o activity)");
        return false;
    }
    if (size == 0 || size > static_cast<size_t>(INT_MAX)) return false;

    jbyteArray bytes = nullptr;
    jclass factory = nullptr;
    jclass options_class = nullptr;
    jobject options = nullptr;
    jclass config_class = nullptr;
    jobject argb = nullptr;
    jobject bitmap = nullptr;

    const auto cleanup = [&]() {
        if (bitmap) env->DeleteLocalRef(bitmap);
        if (argb) env->DeleteLocalRef(argb);
        if (config_class) env->DeleteLocalRef(config_class);
        if (options) env->DeleteLocalRef(options);
        if (options_class) env->DeleteLocalRef(options_class);
        if (factory) env->DeleteLocalRef(factory);
        if (bytes) env->DeleteLocalRef(bytes);
    };
    const auto fail = [&](const char* message) -> bool {
        if (env->ExceptionCheck()) env->ExceptionClear();
        image_detail::warn(message);
        cleanup();
        return false;
    };
    const auto clear_if_exception = [&]() -> bool {
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return false;
        }
        return true;
    };

    bytes = env->NewByteArray(static_cast<jsize>(size));
    if (!bytes) return fail("WebP: NewByteArray falhou");
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(size),
                            reinterpret_cast<const jbyte*>(data));
    if (!clear_if_exception()) return fail("WebP: SetByteArrayRegion falhou");

    factory = env->FindClass("android/graphics/BitmapFactory");
    config_class = env->FindClass("android/graphics/Bitmap$Config");
    options_class = env->FindClass("android/graphics/BitmapFactory$Options");
    if (!factory || !config_class || !options_class || !clear_if_exception()) {
        return fail("WebP: classes Android não encontradas");
    }

    jfieldID argb_field = env->GetStaticFieldID(
        config_class, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    if (!argb_field || !clear_if_exception()) return fail("WebP: Config.ARGB_8888 ausente");
    argb = env->GetStaticObjectField(config_class, argb_field);

    jmethodID options_ctor = env->GetMethodID(options_class, "<init>", "()V");
    if (!options_ctor || !clear_if_exception()) return fail("WebP: construtor de Options ausente");
    options = env->NewObject(options_class, options_ctor);

    jfieldID preferred = env->GetFieldID(
        options_class, "inPreferredConfig", "Landroid/graphics/Bitmap$Config;");
    if (preferred) env->SetObjectField(options, preferred, argb);
    jfieldID scaled = env->GetFieldID(options_class, "inScaled", "Z");
    if (scaled) env->SetBooleanField(options, scaled, JNI_FALSE);
    // Evita halo de premultiplicação com glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
    jfieldID premultiplied = env->GetFieldID(options_class, "inPremultiplied", "Z");
    if (premultiplied) env->SetBooleanField(options, premultiplied, JNI_FALSE);
    if (!clear_if_exception()) return fail("WebP: Options inválidas");

    jmethodID decode = env->GetStaticMethodID(
        factory, "decodeByteArray",
        "([BIILandroid/graphics/BitmapFactory$Options;)Landroid/graphics/Bitmap;");
    if (!decode || !clear_if_exception()) return fail("WebP: decodeByteArray ausente");
    bitmap = env->CallStaticObjectMethod(factory, decode, bytes, 0,
                                         static_cast<jint>(size), options);
    if (!clear_if_exception()) return fail("WebP: BitmapFactory falhou");
    if (!bitmap) return fail("WebP: BitmapFactory retornou null");

    AndroidBitmapInfo info{};
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.width <= 0 || info.height <= 0 ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        return fail("WebP: bitmap não é RGBA_8888");
    }
    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS ||
        !pixels) {
        return fail("WebP: lockPixels falhou");
    }

    const size_t row_bytes = static_cast<size_t>(info.width) * 4;
    out.width = info.width;
    out.height = info.height;
    out.rgba.resize(row_bytes * static_cast<size_t>(info.height));
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    for (int y = 0; y < info.height; ++y) {
        std::memcpy(out.rgba.data() + static_cast<size_t>(y) * row_bytes,
                    src + static_cast<size_t>(y) * info.stride, row_bytes);
    }
    AndroidBitmap_unlockPixels(env, bitmap);

    // Se o device ignorou inPremultiplied=false, desmultiplica aqui.
    if ((info.flags & ANDROID_BITMAP_FLAGS_ALPHA_MASK) ==
        ANDROID_BITMAP_FLAGS_ALPHA_PREMUL) {
        for (size_t offset = 0; offset < out.rgba.size(); offset += 4) {
            const uint8_t alpha = out.rgba[offset + 3];
            if (alpha == 0) {
                out.rgba[offset] = out.rgba[offset + 1] = out.rgba[offset + 2] = 0;
            } else if (alpha != 255) {
                for (int channel = 0; channel < 3; ++channel) {
                    out.rgba[offset + channel] = static_cast<uint8_t>(
                        std::min(255, (out.rgba[offset + channel] * 255) / alpha));
                }
            }
        }
    }

    cleanup();
    return true;
}

bool decode_svg_nano(const uint8_t* data, size_t size, DecodedImage& out,
                     int svg_w, int svg_h) {
    if (!data || size == 0) return false;

    // nsvgParse modifica a string, então trabalhamos numa cópia.
    std::vector<char> text(reinterpret_cast<const char*>(data),
                           reinterpret_cast<const char*>(data) + size);
    text.push_back('\0');
    NSVGimage* image = nsvgParse(text.data(), "px", 96.0f);
    if (!image || image->width <= 0 || image->height <= 0) {
        if (image) nsvgDelete(image);
        image_detail::warn("SVG: documento inválido ou sem dimensões");
        return false;
    }

    const float natural_w = image->width;
    const float natural_h = image->height;
    int width = svg_w > 0 ? svg_w
                          : (svg_h > 0 ? static_cast<int>(std::ceil(svg_h * natural_w / natural_h))
                                       : static_cast<int>(std::ceil(natural_w)));
    int height = svg_h > 0 ? svg_h : static_cast<int>(std::ceil(natural_h * width / natural_w));
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        nsvgDelete(image);
        image_detail::warn("SVG: dimensão de rasterização inválida");
        return false;
    }

    const float scale = std::min(static_cast<float>(width) / natural_w,
                                 static_cast<float>(height) / natural_h);
    const float tx = (static_cast<float>(width) - natural_w * scale) * 0.5f;
    const float ty = (static_cast<float>(height) - natural_h * scale) * 0.5f;

    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);

    NSVGrasterizer* rasterizer = nsvgCreateRasterizer();
    if (!rasterizer) {
        nsvgDelete(image);
        return false;
    }
    nsvgRasterize(rasterizer, image, tx, ty, scale,
                  out.rgba.data(), width, height, width * 4);
    nsvgDeleteRasterizer(rasterizer);
    nsvgDelete(image);
    return true;
}

// ---------------------------------------------------------------------------
// SVG completo (LunaSVG). nano continua como fallback para ícones simples.
// ---------------------------------------------------------------------------

bool decode_svg_full(const uint8_t* data, size_t size, DecodedImage& out,
                     int svg_w, int svg_h) {
#ifdef APKFAST_SVG_FULL
    if (!data || size == 0) return false;
    auto document = lunasvg::Document::loadFromData(
        reinterpret_cast<const char*>(data), size);
    if (!document) return false;

    const float natural_w = document->width();
    const float natural_h = document->height();
    if (natural_w <= 0 || natural_h <= 0) return false;

    int width = svg_w > 0 ? svg_w
                          : (svg_h > 0 ? static_cast<int>(std::ceil(svg_h * natural_w / natural_h))
                                       : static_cast<int>(std::ceil(natural_w)));
    int height = svg_h > 0 ? svg_h : static_cast<int>(std::ceil(natural_h * width / natural_w));
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;

    const float scale = std::min(static_cast<float>(width) / natural_w,
                                 static_cast<float>(height) / natural_h);
    const float tx = (static_cast<float>(width) - natural_w * scale) * 0.5f;
    const float ty = (static_cast<float>(height) - natural_h * scale) * 0.5f;

    lunasvg::Bitmap bitmap(width, height);
    document->render(bitmap, lunasvg::Matrix(scale, 0, 0, scale, tx, ty));
    bitmap.convertToRGBA();

    out.width = width;
    out.height = height;
    out.rgba.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        std::memcpy(out.rgba.data() + static_cast<size_t>(y) * width * 4,
                    bitmap.data() + static_cast<size_t>(y) * bitmap.stride(),
                    static_cast<size_t>(width) * 4);
    }
    return true;
#else
    (void)data; (void)size; (void)out; (void)svg_w; (void)svg_h;
    return false;
#endif
}

bool svg_register_font_asset(const char* family, const std::string& asset,
                             bool bold, bool italic) {
#ifdef APKFAST_SVG_FULL
    if (!family || asset.empty()) return false;
    auto& assets = Assets::instance(nullptr);
    if (!assets.manager()) return false;
    auto bytes = assets.read(asset);
    if (bytes.empty()) return false;
    auto* held = new std::vector<uint8_t>(std::move(bytes));
    return lunasvg_add_font_face_from_data(
        family, bold, italic, held->data(), held->size(),
        [](void* pointer) { delete static_cast<std::vector<uint8_t>*>(pointer); }, held);
#else
    (void)family; (void)asset; (void)bold; (void)italic;
    return false;
#endif
}

bool decode_svg_auto(const uint8_t* data, size_t size, DecodedImage& out,
                     int svg_w, int svg_h) {
#ifdef APKFAST_SVG_FULL
    // Marcadores de recursos avançados: usa LunaSVG; senão mantém NanoSVG
    // (compatibilidade visual com ícones simples da v2.2).
    static const char* markers[] = {
        "<style", "style=", "<text", "<tspan", "clip-path", "mask=",
        "<pattern", "<use", "<marker", "filter=", "<filter", "<image",
        "font-family",
    };
    std::string text(reinterpret_cast<const char*>(data), size);
    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    bool advanced = false;
    for (const char* marker : markers) {
        if (text.find(marker) != std::string::npos) {
            advanced = true;
            break;
        }
    }
    if (advanced && decode_svg_full(data, size, out, svg_w, svg_h)) return true;
#endif
    return decode_svg_nano(data, size, out, svg_w, svg_h);
}

// ---------------------------------------------------------------------------
// WebP animado (libwebp 1.6+, decoder + demux). Sem a lib, JNI puro continua
// cobrindo apenas o primeiro frame.
// ---------------------------------------------------------------------------

bool decode_webp_full(const uint8_t* data, size_t size, DecodedImage& out) {
#ifdef APKFAST_WEBP_FULL
    AnimatedImage animation;
    if (!animation.open(data, size) || animation.frame_count() == 0) return false;
    const auto& first = animation.frame(0);
    out.width = first.width;
    out.height = first.height;
    out.rgba = first.rgba;
    return true;
#else
    (void)data; (void)size; (void)out;
    return false;
#endif
}

AnimatedImage::AnimatedImage() = default;

AnimatedImage::~AnimatedImage() { close(); }

void AnimatedImage::close() {
#ifdef APKFAST_WEBP_FULL
    if (decoder_) {
        WebPAnimDecoderDelete(decoder_);
        decoder_ = nullptr;
    }
    delete data_;
    data_ = nullptr;
#endif
    bytes_.clear();
    frames_.clear();
    index_ = 0;
    elapsed_ms_ = 0.0f;
    plays_ = 0;
    width_ = height_ = 0;
    loop_count_ = 1;
}

bool AnimatedImage::open(const uint8_t* data, size_t size) {
#ifdef APKFAST_WEBP_FULL
    close();
    if (!data || size < 12) return false;

    bytes_.assign(data, data + size);
    data_ = new WebPData{bytes_.data(), bytes_.size()};

    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) {
        close();
        return false;
    }
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;

    decoder_ = WebPAnimDecoderNew(data_, &options);
    if (!decoder_) {
        close();
        return false;
    }
    WebPAnimInfo info{};
    if (!WebPAnimDecoderGetInfo(decoder_, &info)) {
        close();
        return false;
    }
    width_ = static_cast<int>(info.canvas_width);
    height_ = static_cast<int>(info.canvas_height);
    loop_count_ = static_cast<int>(info.loop_count);
    if (width_ <= 0 || height_ <= 0 || info.frame_count == 0 || info.frame_count > 4096) {
        close();
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(width_) * height_ * 4 * info.frame_count;
    if (total > 128ULL * 1024 * 1024) {
        image_detail::warn("WebP animado acima do limite de 128 MiB");
        close();
        return false;
    }

    frames_.reserve(info.frame_count);
    int previous_timestamp = 0;
    while (WebPAnimDecoderHasMoreFrames(decoder_)) {
        uint8_t* buffer = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(decoder_, &buffer, &timestamp)) {
            close();
            return false;
        }
        Frame frame;
        frame.width = width_;
        frame.height = height_;
        frame.duration_ms = std::max(1, timestamp - previous_timestamp);
        previous_timestamp = timestamp;
        frame.rgba.assign(buffer, buffer + static_cast<size_t>(width_) * height_ * 4);
        frames_.push_back(std::move(frame));
    }

    // Os frames já copiaram os pixels; o decoder não é mais necessário.
    WebPAnimDecoderDelete(decoder_);
    decoder_ = nullptr;
    delete data_;
    data_ = nullptr;
    bytes_.clear();
    reset();
    return true;
#else
    (void)data; (void)size;
    return false;
#endif
}

bool AnimatedImage::open_asset(const std::string& path) {
    auto& assets = Assets::instance(nullptr);
    if (!assets.manager()) return false;
    const auto bytes = assets.read(path);
    if (bytes.empty()) return false;
    return open(bytes.data(), bytes.size());
}

const AnimatedImage::Frame& AnimatedImage::frame(size_t index) const {
    static const Frame empty;
    if (frames_.empty()) return empty;
    return frames_[index % frames_.size()];
}

const AnimatedImage::Frame& AnimatedImage::current_frame() const {
    return frame(index_);
}

void AnimatedImage::reset() {
    index_ = 0;
    elapsed_ms_ = 0.0f;
    plays_ = 0;
}

bool AnimatedImage::update(float dt_ms) {
    if (frames_.empty() || dt_ms <= 0.0f) return false;
    const size_t before = index_;
    elapsed_ms_ += dt_ms;

    // Limite de iterações evita duration_ms=0 ou dt enorme travando o loop.
    for (int guard = 0; guard < 1024 && elapsed_ms_ >= frames_[index_].duration_ms; ++guard) {
        elapsed_ms_ -= static_cast<float>(frames_[index_].duration_ms);
        if (index_ + 1 < frames_.size()) {
            ++index_;
            continue;
        }
        ++plays_;
        const bool infinite = force_loop_ || loop_count_ == 0;
        if (infinite || plays_ < static_cast<uint32_t>(loop_count_)) {
            index_ = 0;
        } else {
            index_ = frames_.size() - 1;
            elapsed_ms_ = 0.0f;
            break;
        }
    }
    return index_ != before;
}

}  // namespace nativeapp
